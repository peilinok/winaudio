#include "AudioSessionEnumerator.h"
#include <algorithm>
#include <cwchar>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include "ComUtil.h"
#include "DeviceEnumerator.h"

namespace wa {
namespace {

std::wstring basenameOfPath(const std::wstring& path) {
    const size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

std::wstring processNameFromPid(uint32_t pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"pid-" + std::to_wstring(pid);

    wchar_t path[MAX_PATH] = {};
    DWORD n = static_cast<DWORD>(std::size(path));
    std::wstring name;
    if (QueryFullProcessImageNameW(h, 0, path, &n) && n > 0) {
        name = basenameOfPath(std::wstring(path, n));
    }
    CloseHandle(h);
    return name.empty() ? (L"pid-" + std::to_wstring(pid)) : name;
}

Result openRenderDevice(const DeviceId& id, ComPtr<IMMDevice>& dev) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "CoCreateInstance(MMDeviceEnumerator)");

    hr = e->GetDevice(id.c_str(), dev.GetAddressOf());
    if (FAILED(hr)) return HrToResult(hr, "IMMDeviceEnumerator::GetDevice");
    return Result::Ok();
}

bool processNameLess(const AudioSessionProcess& a, const AudioSessionProcess& b) {
    int cmp = _wcsicmp(a.processName.c_str(), b.processName.c_str());
    if (cmp != 0) return cmp < 0;
    if (a.processName != b.processName) return a.processName < b.processName;
    return a.processId < b.processId;
}

} // namespace

void sortAndDedupeAudioSessionProcesses(std::vector<AudioSessionProcess>& rows) {
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [](const AudioSessionProcess& row) {
                                  return row.processId == 0;
                              }),
               rows.end());

    std::sort(rows.begin(), rows.end(),
              [](const AudioSessionProcess& a, const AudioSessionProcess& b) {
                  if (a.processId != b.processId) return a.processId < b.processId;
                  return processNameLess(a, b);
              });
    rows.erase(std::unique(rows.begin(), rows.end(),
                           [](const AudioSessionProcess& a, const AudioSessionProcess& b) {
                               return a.processId == b.processId;
                           }),
               rows.end());

    std::sort(rows.begin(), rows.end(), processNameLess);
}

bool parseApplicationLoopbackPid(const char* text, uint32_t& pidOut) {
    if (!text) return false;
    while (*text == ' ' || *text == '\t') ++text;
    if (*text < '0' || *text > '9') return false;

    unsigned long long value = 0;
    while (*text >= '0' && *text <= '9') {
        value = value * 10ull + static_cast<unsigned long long>(*text - '0');
        if (value > static_cast<unsigned long long>(UINT32_MAX))
            return false;
        ++text;
    }
    while (*text == ' ' || *text == '\t') ++text;
    if (*text != '\0' || value == 0) {
        return false;
    }
    pidOut = static_cast<uint32_t>(value);
    return true;
}

Result AudioSessionEnumerator::enumerate(std::vector<AudioSessionProcess>& out) {
    out.clear();

    ComInitGuard com;
    if (!com.ok()) return HrToResult(com.hr, "AudioSessionEnumerator: CoInitializeEx");

    DeviceEnumerator devices;
    std::vector<DeviceInfo> renderDevices;
    Result r = devices.enumerate(DataFlow::Render, renderDevices);
    if (!r) return r;

    Result lastFailure = Result::Ok();
    bool sawSuccessfulGetCount = false;
    for (const auto& info : renderDevices) {
        ComPtr<IMMDevice> dev;
        r = openRenderDevice(info.id, dev);
        if (!r) { lastFailure = r; continue; }

        ComPtr<IAudioSessionManager2> manager;
        HRESULT hr = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void**>(manager.GetAddressOf()));
        if (FAILED(hr)) {
            lastFailure = HrToResult(hr, "AudioSessionEnumerator: Activate(IAudioSessionManager2)");
            continue;
        }

        ComPtr<IAudioSessionEnumerator> sessions;
        hr = manager->GetSessionEnumerator(sessions.GetAddressOf());
        if (FAILED(hr)) {
            lastFailure = HrToResult(hr, "AudioSessionEnumerator: GetSessionEnumerator");
            continue;
        }
        int count = 0;
        hr = sessions->GetCount(&count);
        if (FAILED(hr)) {
            lastFailure = HrToResult(hr, "AudioSessionEnumerator: GetCount");
            continue;
        }
        sawSuccessfulGetCount = true;

        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> control;
            hr = sessions->GetSession(i, control.GetAddressOf());
            if (FAILED(hr) || !control) continue;

            ComPtr<IAudioSessionControl2> control2;
            hr = control.As(&control2);
            if (FAILED(hr) || !control2) continue;

            DWORD pid = 0;
            hr = control2->GetProcessId(&pid);
            if (FAILED(hr) || pid == 0) continue;

            out.push_back(AudioSessionProcess{static_cast<uint32_t>(pid),
                                              processNameFromPid(static_cast<uint32_t>(pid))});
        }
    }

    sortAndDedupeAudioSessionProcesses(out);
    if (!sawSuccessfulGetCount && !renderDevices.empty() && !lastFailure)
        return lastFailure;
    return Result::Ok();
}

} // namespace wa
