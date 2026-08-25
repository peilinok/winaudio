#include "LiveSessionEnumerator.h"
#include <cwchar>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "DeviceEnumerator.h"
#include "Log.h"

namespace wa {
namespace {

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                        s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring basenameOfPath(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

std::string processNameFromPid(uint32_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return "pid-" + std::to_string(pid);

    wchar_t path[MAX_PATH] = {};
    DWORD n = static_cast<DWORD>(std::size(path));
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &n) && n > 0) {
        name = basenameOfPath(std::wstring(path, n));
    }
    CloseHandle(h);
    if (name.empty()) return "pid-" + std::to_string(pid);
    return wideToUtf8(name);
}

const char* sessionStateName(AudioSessionState st) {
    switch (st) {
        case AudioSessionStateInactive: return "Inactive";
        case AudioSessionStateActive:   return "Active";
        case AudioSessionStateExpired:  return "Expired";
    }
    return "Unknown";
}

void fillVolumeMute(IAudioSessionControl* control, LiveSessionView& row) {
    ComPtr<ISimpleAudioVolume> vol;
    HRESULT hr = control->QueryInterface(__uuidof(ISimpleAudioVolume),
                                         reinterpret_cast<void**>(vol.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "LiveSession", "QueryInterface(ISimpleAudioVolume)", "",
           wa::log::hrName(hr));
    if (FAILED(hr) || !vol) return;

    float level = 1.f;
    hr = vol->GetMasterVolume(&level);
    WA_LOG(wa::log::Level::Debug, "LiveSession", "GetMasterVolume", "", wa::log::hrName(hr));
    if (SUCCEEDED(hr)) row.sessionVolume = level;

    BOOL mute = FALSE;
    hr = vol->GetMute(&mute);
    WA_LOG(wa::log::Level::Debug, "LiveSession", "GetMute", "", wa::log::hrName(hr));
    if (SUCCEEDED(hr)) row.sessionMute = mute != FALSE;
}

Result appendDeviceSessions(const DeviceInfo& info, std::vector<LiveSessionView>& out,
                            Result& lastFailure, bool& sawCount) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "LiveSession", "CoCreateInstance(MMDeviceEnumerator)", "",
           wa::log::hrName(hr));
    if (FAILED(hr)) {
        lastFailure = HrToResult(hr, "LiveSessionEnumerator: CoCreateInstance(MMDeviceEnumerator)");
        return lastFailure;
    }

    ComPtr<IMMDevice> dev;
    hr = e->GetDevice(info.id.c_str(), dev.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "LiveSession", "GetDevice",
           "id=" + wa::narrowAscii(info.id), wa::log::hrName(hr));
    if (FAILED(hr)) {
        lastFailure = HrToResult(hr, "LiveSessionEnumerator: GetDevice");
        return Result::Ok();
    }

    ComPtr<IAudioSessionManager2> manager;
    hr = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                       reinterpret_cast<void**>(manager.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "LiveSession", "Activate(IAudioSessionManager2)",
           "id=" + wa::narrowAscii(info.id), wa::log::hrName(hr));
    if (FAILED(hr)) {
        lastFailure = HrToResult(hr, "LiveSessionEnumerator: Activate(IAudioSessionManager2)");
        return Result::Ok();
    }

    ComPtr<IAudioSessionEnumerator> sessions;
    hr = manager->GetSessionEnumerator(sessions.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "LiveSession", "GetSessionEnumerator", "", wa::log::hrName(hr));
    if (FAILED(hr)) {
        lastFailure = HrToResult(hr, "LiveSessionEnumerator: GetSessionEnumerator");
        return Result::Ok();
    }

    int count = 0;
    hr = sessions->GetCount(&count);
    WA_LOG(wa::log::Level::Debug, "LiveSession", "GetCount",
           "n=" + std::to_string(count), wa::log::hrName(hr));
    if (FAILED(hr)) {
        lastFailure = HrToResult(hr, "LiveSessionEnumerator: GetCount");
        return Result::Ok();
    }
    sawCount = true;

    const PipelineFlow flow =
        info.flow == DataFlow::Capture ? PipelineFlow::Capture : PipelineFlow::Render;
    const std::string deviceId = wideToUtf8(info.id);
    const std::string deviceName = wideToUtf8(info.name);

    for (int i = 0; i < count; ++i) {
        ComPtr<IAudioSessionControl> control;
        hr = sessions->GetSession(i, control.GetAddressOf());
        WA_LOG(wa::log::Level::Debug, "LiveSession", "GetSession",
               "i=" + std::to_string(i), wa::log::hrName(hr));
        if (FAILED(hr) || !control) {
            if (FAILED(hr))
                WA_LOG(wa::log::Level::Warn, "LiveSession", "GetSession",
                       "i=" + std::to_string(i), wa::log::hrName(hr));
            continue;
        }

        ComPtr<IAudioSessionControl2> control2;
        hr = control.As(&control2);
        WA_LOG(wa::log::Level::Debug, "LiveSession", "QueryInterface(IAudioSessionControl2)",
               "i=" + std::to_string(i), wa::log::hrName(hr));
        if (FAILED(hr) || !control2) {
            if (FAILED(hr))
                WA_LOG(wa::log::Level::Warn, "LiveSession", "QueryInterface(IAudioSessionControl2)",
                       "i=" + std::to_string(i), wa::log::hrName(hr));
            continue;
        }

        DWORD pid = 0;
        hr = control2->GetProcessId(&pid);
        WA_LOG(wa::log::Level::Debug, "LiveSession", "GetProcessId", "", wa::log::hrName(hr));
        if (FAILED(hr) || pid == 0) continue;

        AudioSessionState st = AudioSessionStateExpired;
        hr = control->GetState(&st);
        WA_LOG(wa::log::Level::Debug, "LiveSession", "GetState", "", wa::log::hrName(hr));

        LiveSessionView row;
        row.processId = static_cast<uint32_t>(pid);
        row.processName = processNameFromPid(row.processId);
        row.deviceId = deviceId;
        row.deviceName = deviceName;
        row.flow = flow;
        row.state = sessionStateName(st);
        fillVolumeMute(control.Get(), row);
        out.push_back(std::move(row));
    }
    return Result::Ok();
}

}  // namespace

Result LiveSessionEnumerator::enumerate(std::vector<LiveSessionView>& out) {
    out.clear();
    WA_LOG(wa::log::Level::Info, "LiveSession", "enumerate", "", "start");

    ComInitGuard com;
    if (!com.ok()) return HrToResult(com.hr, "LiveSessionEnumerator: CoInitializeEx");

    DeviceEnumerator devices;
    std::vector<DeviceInfo> caps;
    std::vector<DeviceInfo> rens;
    Result r = devices.enumerate(DataFlow::Capture, caps);
    if (!r) return r;
    r = devices.enumerate(DataFlow::Render, rens);
    if (!r) return r;

    Result lastFailure = Result::Ok();
    bool sawCount = false;
    for (const auto& info : caps) {
        appendDeviceSessions(info, out, lastFailure, sawCount);
    }
    for (const auto& info : rens) {
        appendDeviceSessions(info, out, lastFailure, sawCount);
    }

    shapeLiveSessionList(out, 0);
    if (!sawCount && (!caps.empty() || !rens.empty()) && !lastFailure)
        return lastFailure;
    WA_LOG(wa::log::Level::Info, "LiveSession", "enumerate",
           "n=" + std::to_string(out.size()), "ok");
    return Result::Ok();
}

}  // namespace wa
