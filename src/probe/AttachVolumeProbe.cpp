// Manual, device-dependent regression probe: exercise the real target vtables.
#include "OnDemandAttach.h"
#include "HookModuleIdentity.h"
#include "Log.h"
#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>
#include <cstdio>
#include <string>

namespace {
using Microsoft::WRL::ComPtr;
constexpr const char* kMethods[] = {
    "GetMasterVolume", "SetMasterVolume", "GetMute", "SetMute"
};
constexpr const char* kArgs[] = {"level=0.625", "level=0.500", "false", "true"};

struct CallResult {
    HRESULT hr = E_PENDING;
    HRESULT readback = E_PENDING;
    float level = -1;
    BOOL mute = -1;
    bool completed = false;
};

HRESULT observe(const char* operation, HRESULT hr, const std::string& args = {},
                wa::log::Level level = wa::log::Level::Debug) {
    WA_LOG(FAILED(hr) ? wa::log::Level::Warn : level,
           "AttachVolumeProbe", operation, args, wa::log::hrName(hr));
    return hr;
}

std::string handleArgs(HANDLE handle) {
    return "handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle));
}

std::string eventArgs(const wchar_t* name, DWORD access) {
    char args[512]{};
    sprintf_s(args, "name=%ls access=0x%08lX inherit=FALSE", name, access);
    return args;
}

void observeWait(const char* operation, DWORD outcome, DWORD error, const std::string& args,
                 wa::log::Level level = wa::log::Level::Debug) {
    const char* state = outcome == WAIT_OBJECT_0 ? "WAIT_OBJECT_0/signaled" :
        outcome == WAIT_OBJECT_0 + 1 ? "WAIT_OBJECT_0+1/target-exited" :
        outcome == WAIT_TIMEOUT ? "WAIT_TIMEOUT" : outcome == WAIT_FAILED ? "WAIT_FAILED" : "other";
    const HRESULT hr = outcome == WAIT_FAILED ? HRESULT_FROM_WIN32(error) :
        outcome == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : S_OK;
    WA_LOG(FAILED(hr) ? wa::log::Level::Warn : level, "AttachVolumeProbe", operation, args,
           std::string(state) + " result=" + std::to_string(outcome) + " " + wa::log::hrName(hr));
}

void closeHandle(const char* operation, HANDLE handle) {
    const BOOL closed = CloseHandle(handle);
    const DWORD error = closed ? ERROR_SUCCESS : GetLastError();
    observe(operation, HRESULT_FROM_WIN32(error), handleArgs(handle));
}

template<class Call>
bool win32Call(const char* operation, Call call, const std::string& args) {
    const bool success = call();
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    observe(operation, HRESULT_FROM_WIN32(error), args);
    return success;
}

// The parent retains the mapping until the owned child has exited.
class ResultMapping {
public:
    explicit ResultMapping(const wchar_t* readyName) {
        const std::wstring name = std::wstring(readyName) + L"-result";
        if (win32Call("CreateFileMappingW/result", [&] {
                handle_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                            sizeof(CallResult), name.c_str());
                return handle_ != nullptr;
            }, "pagefile; PAGE_READWRITE; bytes=" + std::to_string(sizeof(CallResult))))
            win32Call("MapViewOfFile/result", [&] {
                result = static_cast<CallResult*>(MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CallResult)));
                return result != nullptr;
            }, "handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle_)) +
               "; FILE_MAP_ALL_ACCESS; bytes=" + std::to_string(sizeof(CallResult)));
    }
    ~ResultMapping() {
        if (result) win32Call("UnmapViewOfFile/result", [&] { return UnmapViewOfFile(result) != FALSE; },
                              "address=" + std::to_string(reinterpret_cast<uintptr_t>(result)));
        if (handle_) win32Call("CloseHandle/result", [&] { return CloseHandle(handle_) != FALSE; },
                               "handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle_)));
    }
    CallResult* result = nullptr;
private:
    HANDLE handle_ = nullptr;
};

bool loadedIdentity(DWORD pid, const wchar_t* self) {
    std::wstring source(self);
    source.resize(source.find_last_of(L"\\/") + 1);
    source += L"WinAudioHook.dll";
    std::wstring expected;
    const auto staged = wa::stageHookModule(source, expected);
    bool loaded = false;
    if (!staged || !wa::inspectHookModules(pid, expected, loaded) || !loaded) return false;
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    if (!win32Call("CreateToolhelp32Snapshot", [&] {
            snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
            return snapshot != INVALID_HANDLE_VALUE;
        }, "TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32; pid=" + std::to_string(pid))) return false;
    MODULEENTRY32W module{sizeof(module)};
    bool found = false;
    if (win32Call("Module32FirstW", [&] { return Module32FirstW(snapshot, &module) != FALSE; },
                  "pid=" + std::to_string(pid))) do {
        if (wa::isHookModuleName(module.szModule)) {
            std::printf("loaded pid=%lu path=%ls expected=%ls\n", pid, module.szExePath, expected.c_str());
            found = true;
        }
        const BOOL next = Module32NextW(snapshot, &module);
        const DWORD error = next ? ERROR_SUCCESS : GetLastError();
        WA_LOG(error != ERROR_SUCCESS && error != ERROR_NO_MORE_FILES ? wa::log::Level::Warn : wa::log::Level::Debug,
               "AttachVolumeProbe", "Module32NextW", "pid=" + std::to_string(pid),
               "success=" + std::to_string(next) + " error=" + std::to_string(error) +
               " " + wa::log::hrName(HRESULT_FROM_WIN32(error)));
        if (!next) {
            found = found && error == ERROR_NO_MORE_FILES;
            break;
        }
    } while (true);
    win32Call("CloseHandle/snapshot", [&] { return CloseHandle(snapshot) != FALSE; }, "pid=" + std::to_string(pid));
    return found;
}

HRESULT playAndCall(unsigned method, HANDLE ready, HANDLE go, CallResult& result) {
    ComPtr<IMMDeviceEnumerator> enumer;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    ComPtr<ISimpleAudioVolume> volume;
    HRESULT hr = observe("CoCreateInstance", CoCreateInstance(__uuidof(MMDeviceEnumerator),
        nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumer)), "MMDeviceEnumerator; CLSCTX_ALL; IMMDeviceEnumerator");
    if (FAILED(hr)) return hr;
    hr = observe("GetDefaultAudioEndpoint", enumer->GetDefaultAudioEndpoint(eRender, eConsole, &device),
                 "flow=eRender role=eConsole", wa::log::Level::Info);
    if (FAILED(hr)) return hr;
    hr = observe("Activate", device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(client.GetAddressOf())), "iid=IAudioClient CLSCTX_ALL activationParams=nullptr");
    if (FAILED(hr)) return hr;
    WAVEFORMATEX* format = nullptr;
    hr = observe("GetMixFormat", client->GetMixFormat(&format), "out=WAVEFORMATEX**");
    if (FAILED(hr)) return hr;
    GUID session{};
    hr = observe("CoCreateGuid", CoCreateGuid(&session), "out=session");
    char initArgs[256]{};
    sprintf_s(initArgs, "share=AUDCLNT_SHAREMODE_SHARED flags=0 duration=1000000 periodicity=0 "
              "rate=%lu channels=%u bits=%u tag=0x%04X session=%p", format->nSamplesPerSec,
              format->nChannels, format->wBitsPerSample, format->wFormatTag, static_cast<void*>(&session));
    if (SUCCEEDED(hr)) hr = observe("Initialize", client->Initialize(
        AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, &session), initArgs, wa::log::Level::Info);
    CoTaskMemFree(format);
    WA_LOG(wa::log::Level::Debug, "AttachVolumeProbe", "CoTaskMemFree", "mix format", "void");
    if (FAILED(hr)) return hr;
    hr = observe("GetService/render", client->GetService(IID_PPV_ARGS(&render)), "iid=IAudioRenderClient");
    if (FAILED(hr)) return hr;
    hr = observe("GetService/volume", client->GetService(IID_PPV_ARGS(&volume)), "iid=ISimpleAudioVolume");
    if (FAILED(hr)) return hr;
    // A fresh session keeps setup isolated from other applications.
    hr = observe("SetMasterVolume/setup", volume->SetMasterVolume(0.625f, nullptr), "level=0.625 context=nullptr");
    if (FAILED(hr)) return hr;
    hr = observe("SetMute/setup", volume->SetMute(FALSE, nullptr), "mute=FALSE context=nullptr");
    if (FAILED(hr)) return hr;
    UINT32 capacity = 0;
    hr = observe("GetBufferSize", client->GetBufferSize(&capacity), "out=capacity");
    if (FAILED(hr)) return hr;
    hr = observe("Start", client->Start(), "IAudioClient; no arguments", wa::log::Level::Info);
    if (FAILED(hr)) return hr;
    if (!win32Call("SetEvent/ready", [&] { return SetEvent(ready) != FALSE; }, handleArgs(ready))) return E_FAIL;
    const ULONGLONG deadline = GetTickCount64() + 15000;
    DWORD goWait = WAIT_TIMEOUT;
    while ((goWait = WaitForSingleObject(go, 0)) == WAIT_TIMEOUT) {
        if (GetTickCount64() >= deadline) return observe("WaitForSingleObject/go deadline",
            HRESULT_FROM_WIN32(ERROR_TIMEOUT), "go event; poll=0 deadline=15000ms");
        UINT32 padding = 0;
        hr = client->GetCurrentPadding(&padding);
        if (FAILED(hr)) return observe("GetCurrentPadding", hr);
        if (capacity > padding) {
            BYTE* data = nullptr;
            hr = render->GetBuffer(capacity - padding, &data);
            if (FAILED(hr)) return observe("GetBuffer", hr);
            hr = render->ReleaseBuffer(capacity - padding, AUDCLNT_BUFFERFLAGS_SILENT);
            if (FAILED(hr)) return observe("ReleaseBuffer", hr);
        }
        Sleep(5);
    }
    const DWORD goError = goWait == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    observeWait("WaitForSingleObject/go", goWait, goError, handleArgs(go) + " timeout=0");
    float level = 0;
    BOOL mute = FALSE;
    switch (method) {
    case 0: hr = volume->GetMasterVolume(&level); break;
    case 1: hr = volume->SetMasterVolume(0.5f, nullptr); break;
    case 2: hr = volume->GetMute(&mute); break;
    default: hr = volume->SetMute(TRUE, nullptr); break;
    }
    observe(kMethods[method], hr, "expected=" + std::string(kArgs[method]) + " context=nullptr");
    result.hr = hr;
    result.level = level;
    result.mute = mute;
    result.readback = S_OK;
    if (method == 1) result.readback = observe("GetMasterVolume/readback", volume->GetMasterVolume(&result.level), "out=level");
    if (method == 3) result.readback = observe("GetMute/readback", volume->GetMute(&result.mute), "out=mute");
    result.completed = true;
    const HRESULT stopped = observe("Stop", client->Stop(), "IAudioClient; no arguments", wa::log::Level::Info);
    return FAILED(hr) ? hr : stopped;
}

int target(unsigned method, const wchar_t* readyName, const wchar_t* goName) {
    ResultMapping result(readyName);
    if (!result.result || method >= 4) return 2;
    HANDLE ready = nullptr, go = nullptr;
    win32Call("OpenEventW/ready", [&] {
        ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
        return ready != nullptr;
    }, eventArgs(readyName, EVENT_MODIFY_STATE));
    win32Call("OpenEventW/go", [&] {
        go = OpenEventW(SYNCHRONIZE, FALSE, goName);
        return go != nullptr;
    }, eventArgs(goName, SYNCHRONIZE));
    if (!ready || !go) {
        if (go) closeHandle("CloseHandle/go", go);
        if (ready) closeHandle("CloseHandle/ready", ready);
        return 2;
    }
    const HRESULT com = observe("CoInitializeEx", CoInitializeEx(nullptr, COINIT_MULTITHREADED),
                                "reserved=nullptr COINIT_MULTITHREADED", wa::log::Level::Info);
    const HRESULT hr = SUCCEEDED(com) ? playAndCall(method, ready, go, *result.result) : com;
    if (SUCCEEDED(com)) {
        CoUninitialize();
        WA_LOG(wa::log::Level::Info, "AttachVolumeProbe", "CoUninitialize", "current thread", "void");
    }
    closeHandle("CloseHandle/go", go);
    closeHandle("CloseHandle/ready", ready);
    return FAILED(hr) ? 3 : 0;
}

bool run(unsigned method, bool hooked) {
    const std::wstring prefix = L"Local\\WinAudioAttachVolume-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(method);
    const std::wstring readyName = prefix + L"-ready", goName = prefix + L"-go";
    ResultMapping result(readyName.c_str());
    if (!result.result) return false;
    *result.result = CallResult{};
    HANDLE ready = nullptr, go = nullptr;
    const bool readyCreated = win32Call("CreateEventW/ready", [&] {
        ready = CreateEventW(nullptr, TRUE, FALSE, readyName.c_str());
        return ready != nullptr;
    }, eventArgs(readyName.c_str(), EVENT_ALL_ACCESS) + " manualReset=TRUE initialState=FALSE");
    const bool goCreated = win32Call("CreateEventW/go", [&] {
        go = CreateEventW(nullptr, TRUE, FALSE, goName.c_str());
        return go != nullptr;
    }, eventArgs(goName.c_str(), EVENT_ALL_ACCESS) + " manualReset=TRUE initialState=FALSE");
    if (!readyCreated || !goCreated) {
        if (go) closeHandle("CloseHandle/go", go);
        if (ready) closeHandle("CloseHandle/ready", ready);
        return false;
    }
    wchar_t self[MAX_PATH]{};
    if (!win32Call("GetModuleFileNameW", [&] { return GetModuleFileNameW(nullptr, self, MAX_PATH) != 0; },
                   "module=nullptr capacity=MAX_PATH")) {
        closeHandle("CloseHandle/go", go);
        closeHandle("CloseHandle/ready", ready);
        return false;
    }
    std::wstring command = L"\"" + std::wstring(self) + L"\" --target " +
        std::to_wstring(method) + L" \"" + readyName + L"\" \"" + goName + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!win32Call("CreateProcessW", [&] { return CreateProcessW(nullptr, command.data(), nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE; },
            "self target; method=" + std::to_string(method) + " CREATE_NO_WINDOW inherit=FALSE cwd/environment=parent")) {
        closeHandle("CloseHandle/go", go);
        closeHandle("CloseHandle/ready", ready);
        return false;
    }
    std::printf("target_command=%ls\n", command.c_str());
    FILETIME created{}, exited{}, kernel{}, user{};
    const bool identityKnown = win32Call("GetProcessTimes/target identity", [&] {
        return GetProcessTimes(process.hProcess, &created, &exited, &kernel, &user) != FALSE;
    }, "pid=" + std::to_string(process.dwProcessId));
    const uint64_t creationTime = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    std::printf("target_identity pid=%lu created=%llu verified=%d\n", process.dwProcessId, creationTime,
                identityKnown && creationTime != 0);
    closeHandle("CloseHandle/target thread", process.hThread);
    const HANDLE waits[] = {ready, process.hProcess};
    const DWORD readiness = WaitForMultipleObjects(2, waits, FALSE, 10000);
    const DWORD readinessError = readiness == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    observeWait("WaitForMultipleObjects/ready", readiness, readinessError,
                "ready=" + std::to_string(reinterpret_cast<uintptr_t>(ready)) +
                " target=" + std::to_string(process.dwProcessId) + " waitAll=FALSE timeout=10000", wa::log::Level::Info);
    bool ok = readiness == WAIT_OBJECT_0 && identityKnown && creationTime != 0;
    wa::OnDemandAttach attach;
    if (ok && hooked) {
        const auto attached = attach.start(process.dwProcessId, {}, wa::PipelineFlow::Render,
            nullptr, {}, wa::kDefaultMonitorMethods | wa::methodBit(wa::MonitorMethod::GetMasterVolume) |
                wa::methodBit(wa::MonitorMethod::GetMute));
        ok = static_cast<bool>(attached);
        if (!ok) std::printf("attach_error=%s\n", attached.message.c_str());
        if (ok) ok = loadedIdentity(process.dwProcessId, self);
    }
    ok = win32Call("SetEvent/go", [&] { return SetEvent(go) != FALSE; }, handleArgs(go)) && ok;
    const DWORD waited = WaitForSingleObject(process.hProcess, 10000);
    const DWORD waitError = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    observeWait("WaitForSingleObject/target", waited, waitError,
                "pid=" + std::to_string(process.dwProcessId) + " timeout=10000");
    DWORD code = STILL_ACTIVE;
    ok = win32Call("GetExitCodeProcess", [&] { return GetExitCodeProcess(process.hProcess, &code) != FALSE; },
                   "pid=" + std::to_string(process.dwProcessId)) && ok;
    if (waited != WAIT_OBJECT_0) {
        // Only this probe's owned child can be terminated on timeout.
        win32Call("TerminateProcess/owned target", [&] { return TerminateProcess(process.hProcess, 4) != FALSE; },
                  "pid=" + std::to_string(process.dwProcessId) + " exit=4");
        const DWORD cleaned = WaitForSingleObject(process.hProcess, 5000);
        const DWORD cleanupError = cleaned == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
        observeWait("WaitForSingleObject/cleanup", cleaned, cleanupError,
                    "pid=" + std::to_string(process.dwProcessId) + " timeout=5000");
        DWORD cleanupCode = STILL_ACTIVE;
        win32Call("GetExitCodeProcess/cleanup", [&] { return GetExitCodeProcess(process.hProcess, &cleanupCode) != FALSE; },
                  "pid=" + std::to_string(process.dwProcessId));
        std::printf("cleanup pid=%lu wait=%lu original_exit=0x%08lX exit=0x%08lX\n",
                    process.dwProcessId, cleaned, code, cleanupCode);
        // Do not read shared results while the child may still be writing.
        if (cleaned != WAIT_OBJECT_0) {
            attach.stop();
            closeHandle("CloseHandle/target", process.hProcess);
            closeHandle("CloseHandle/go", go);
            closeHandle("CloseHandle/ready", ready);
            return false;
        }
    }
    auto calls = attach.drain();
    const auto recent = attach.pumpRing();
    calls.insert(calls.end(), recent.begin(), recent.end());
    std::printf("method=%s attached=%d pid=%lu wait=%lu exit=0x%08lX calls=%zu\n",
                kMethods[method], attach.attached(), process.dwProcessId, waited, code, calls.size());
    unsigned matchingCalls = 0;
    for (const auto& call : calls) {
        std::printf("  %s::%s hr=0x%08X args=%s\n", call.iface.c_str(), call.method.c_str(),
                    static_cast<unsigned>(call.hresult), call.args.c_str());
        if (call.iface == "ISimpleAudioVolume" && call.method == kMethods[method] &&
            call.hresult == result.result->hr && call.args == kArgs[method]) ++matchingCalls;
    }
    const auto& observed = *result.result;
    const bool outputMatches = method < 2 ? observed.level == (method == 0 ? 0.625f : 0.5f) :
        observed.mute == (method == 2 ? FALSE : TRUE);
    const bool behaviorMatches = observed.completed && observed.hr == S_OK && observed.readback == S_OK && outputMatches;
    std::printf("  target_result completed=%d hr=0x%08lX readback_hr=0x%08lX level=%.3f mute=%d "
                "behavior_match=%d matching_calls=%u\n", observed.completed, observed.hr,
                observed.readback, static_cast<double>(observed.level), observed.mute, behaviorMatches, matchingCalls);
    std::fflush(stdout);
    attach.stop();
    closeHandle("CloseHandle/target", process.hProcess);
    closeHandle("CloseHandle/go", go);
    closeHandle("CloseHandle/ready", ready);
    return ok && waited == WAIT_OBJECT_0 && code == 0 && behaviorMatches && (!hooked || matchingCalls == 1);
}
}

int wmain(int argc, wchar_t** argv) {
    wa::log::init();
    wa::log::addStderrSink();
    if (argc == 5 && !std::wcscmp(argv[1], L"--target")) {
        const int code = target(static_cast<unsigned>(std::wcstoul(argv[2], nullptr, 10)), argv[3], argv[4]);
        wa::log::shutdown();
        return code;
    }
    bool hooked = true;
    unsigned first = 0, last = 4;
    for (int i = 1; i < argc; ++i) {
        if (!std::wcscmp(argv[i], L"--no-attach")) hooked = false;
        else if (!std::wcscmp(argv[i], L"--method") && i + 1 < argc) {
            first = static_cast<unsigned>(std::wcstoul(argv[++i], nullptr, 10));
            if (first >= 4) return 5;
            last = first + 1;
        } else return 5;
    }
    bool ok = true;
    for (unsigned method = first; method < last; ++method)
        if (!run(method, hooked)) ok = false;
    wa::log::shutdown();
    return ok ? 0 : 1;
}
