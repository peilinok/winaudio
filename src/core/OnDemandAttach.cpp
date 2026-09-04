#include "OnDemandAttach.h"
#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "HookedCallPod.h"
#include "Log.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstring>
#include <string>

namespace wa {
namespace {

std::string basenameUtf8(const wchar_t* path) {
    const wchar_t* slash = path;
    for (const wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') slash = p + 1;
    }
    std::string s;
    for (const wchar_t* p = slash; *p; ++p)
        s += static_cast<char>(*p);
    return s;
}

std::string processName(HANDLE proc) {
    wchar_t path[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    if (!QueryFullProcessImageNameW(proc, 0, path, &n) || n == 0)
        return {};
    return basenameUtf8(path);
}

bool sameBitness(HANDLE proc) {
    BOOL selfWow = FALSE;
    BOOL targetWow = FALSE;
    IsWow64Process(GetCurrentProcess(), &selfWow);
    IsWow64Process(proc, &targetWow);
    return selfWow == targetWow;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring hookDllPath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = path;
    for (wchar_t* p = path; *p; ++p) {
        if (*p == L'\\' || *p == L'/') slash = p;
    }
    if (*slash) {
        slash[1] = 0;
        wcscat_s(path, L"WinAudioHook.dll");
    }
    return path;
}

std::wstring stageHookDll() {
    const std::wstring src = hookDllPath();
    const auto slash = src.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return src;
    std::wstring dst = src.substr(0, slash + 1) + stagedHookFileName();
    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        const DWORD err = GetLastError();
        WA_LOG(wa::log::Level::Warn, "Attach", "CopyFile(stage hook)", wa::narrowAscii(dst),
               wa::log::hrName(HRESULT_FROM_WIN32(err)));
        if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) return src;
    }
    return dst;
}

void enableDebugPrivilege() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
        WA_LOG(wa::log::Level::Debug, "Attach", "OpenProcessToken", "",
               wa::log::hrName(HRESULT_FROM_WIN32(GetLastError())));
        return;
    }
    LUID luid{};
    if (!LookupPrivilegeValueW(nullptr, SE_DEBUG_NAME, &luid)) {
        WA_LOG(wa::log::Level::Debug, "Attach", "LookupPrivilegeValueW(SeDebugPrivilege)", "",
               wa::log::hrName(HRESULT_FROM_WIN32(GetLastError())));
        CloseHandle(tok);
        return;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    AdjustTokenPrivileges(tok, FALSE, &tp, 0, nullptr, nullptr);
    const DWORD err = GetLastError();
    WA_LOG(err == ERROR_SUCCESS ? wa::log::Level::Debug : wa::log::Level::Warn, "Attach",
           "AdjustTokenPrivileges(SeDebugPrivilege)", "",
           wa::log::hrName(HRESULT_FROM_WIN32(err)));
    CloseHandle(tok);
}

HookedCall fromPod(const hook_ipc::CallPod& p) {
    HookedCall c;
    c.streamId = p.streamId;
    c.timeMs = p.timeMs;
    c.iface = p.iface;
    c.method = p.method;
    c.args = p.args;
    c.hresult = p.hresult;
    c.pump = p.pump != 0;
    c.xrun = p.xrun != 0;
    if (p.hasCategory) c.category = std::string(p.category);
    if (p.hasRaw) c.raw = p.raw != 0;
    if (p.hasMatchFormat) c.matchFormat = p.matchFormat != 0;
    if (p.hasExclusive) c.exclusive = p.exclusive != 0;
    if (p.hasFormat) c.format = std::string(p.format);
    return c;
}

Result win32Result(DWORD err, const char* where) {
    if (err == ERROR_SUCCESS) return Result::Ok();
    return HrToResult(HRESULT_FROM_WIN32(err), where);
}

Result injectDll(HANDLE proc, const std::wstring& dll) {
    const size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    const DWORD allocErr = remote ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "VirtualAllocEx", wa::narrowAscii(dll),
           wa::log::hrName(HRESULT_FROM_WIN32(allocErr)));
    if (!remote) return win32Result(allocErr, "Attach: VirtualAllocEx");

    SIZE_T written = 0;
    if (!WriteProcessMemory(proc, remote, dll.c_str(), bytes, &written)) {
        const DWORD err = GetLastError();
        WA_LOG(wa::log::Level::Debug, "Attach", "WriteProcessMemory", wa::narrowAscii(dll),
               wa::log::hrName(HRESULT_FROM_WIN32(err)));
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return win32Result(err, "Attach: WriteProcessMemory");
    }
    WA_LOG(wa::log::Level::Debug, "Attach", "WriteProcessMemory", wa::narrowAscii(dll), "S_OK");

    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto load = k32 ? GetProcAddress(k32, "LoadLibraryW") : nullptr;
    WA_LOG(wa::log::Level::Debug, "Attach", "GetProcAddress(LoadLibraryW)", "",
           load ? "S_OK" : "null");
    if (!load) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return Result::Fail(static_cast<long>(E_FAIL), "Attach: LoadLibraryW not found");
    }

    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(load),
                                       remote, 0, nullptr);
    const DWORD thrErr = thread ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "CreateRemoteThread(LoadLibraryW)",
           wa::narrowAscii(dll), wa::log::hrName(HRESULT_FROM_WIN32(thrErr)));
    if (!thread) {
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        return win32Result(thrErr, "Attach: CreateRemoteThread");
    }
    const DWORD wait = WaitForSingleObject(thread, 10000);
    DWORD exitCode = 0;
    GetExitCodeThread(thread, &exitCode);
    WA_LOG(wait == WAIT_OBJECT_0 ? wa::log::Level::Debug : wa::log::Level::Warn, "Attach",
           "WaitForSingleObject(inject)", "exit=" + std::to_string(exitCode),
           wait == WAIT_OBJECT_0 ? "S_OK" : wa::log::hrName(HRESULT_FROM_WIN32(wait)));
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (!remoteLoadLibrarySucceeded(wait, exitCode)) {
        if (wait != WAIT_OBJECT_0)
            return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)),
                                "Attach: inject timed out");
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)),
                            "Attach: LoadLibraryW failed in target");
    }
    return Result::Ok();
}

}  // namespace

const char* attachBlockText(AttachBlock block) {
    switch (block) {
        case AttachBlock::None:          return "Attached";
        case AttachBlock::PidZero:       return "Attach failed: invalid PID";
        case AttachBlock::SelfProcess:   return "Attach failed: cannot attach to this process";
        case AttachBlock::Audiodg:       return "Attach failed: refusing audiodg";
        case AttachBlock::CrossBitness:  return "Attach failed: cross-bitness";
        case AttachBlock::NoDebugRights: return "Attach failed: missing debug rights";
    }
    return "Attach failed";
}

bool remoteLoadLibrarySucceeded(uint32_t waitResult, uint32_t exitCode) {
    return waitResult == WAIT_OBJECT_0 && exitCode != 0;
}

const char* attachInstallFailMessage(uint32_t installed) {
    return installed == hook_ipc::kInstallFailed
               ? "Attach: hook install failed in target"
               : "Attach: hook install did not finish; restart the target app and retry";
}

std::wstring stagedHookFileName() {
    return L"WinAudioHook-" + std::to_wstring(hook_ipc::kRingLayout) + L".dll";
}

AttachBlock evaluateAttach(uint32_t pid, uint32_t ourPid, bool sameBitnessFlag, bool hasDebugRights,
                           const std::string& processName) {
    if (pid == 0) return AttachBlock::PidZero;
    if (pid == ourPid) return AttachBlock::SelfProcess;
    if (_stricmp(processName.c_str(), "audiodg.exe") == 0) return AttachBlock::Audiodg;
    if (!sameBitnessFlag) return AttachBlock::CrossBitness;
    if (!hasDebugRights) return AttachBlock::NoDebugRights;
    return AttachBlock::None;
}

struct OnDemandAttach::Impl {
    uint32_t pid = 0;
    AttachBlock block = AttachBlock::None;
    HANDLE map = nullptr;
    hook_ipc::Ring* ring = nullptr;
    uint32_t readIndex = 0;
};

OnDemandAttach::OnDemandAttach() : impl_(std::make_unique<Impl>()) {}
OnDemandAttach::~OnDemandAttach() { stop(); }

void OnDemandAttach::stop() {
    if (!impl_) return;
    if (impl_->ring) {
        UnmapViewOfFile(impl_->ring);
        impl_->ring = nullptr;
    }
    if (impl_->map) {
        CloseHandle(impl_->map);
        impl_->map = nullptr;
    }
    if (impl_->pid) {
        WA_LOG(wa::log::Level::Info, "Attach", "stop", "pid=" + std::to_string(impl_->pid), "ok");
    }
    impl_->pid = 0;
    impl_->readIndex = 0;
}

bool OnDemandAttach::attached() const {
    return impl_ && impl_->ring && impl_->pid != 0 && impl_->block == AttachBlock::None;
}

uint32_t OnDemandAttach::pid() const { return impl_ ? impl_->pid : 0; }

AttachBlock OnDemandAttach::lastBlock() const {
    return impl_ ? impl_->block : AttachBlock::PidZero;
}

Result OnDemandAttach::start(uint32_t pid, const std::string& deviceIdUtf8, PipelineFlow flow) {
    stop();
    impl_->block = AttachBlock::PidZero;
    enableDebugPrivilege();
    const uint32_t ourPid = GetCurrentProcessId();

    HANDLE query = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    const DWORD queryErr = query ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "OpenProcess(QUERY)", "pid=" + std::to_string(pid),
           wa::log::hrName(HRESULT_FROM_WIN32(queryErr)));
    if (!query) {
        impl_->block = AttachBlock::NoDebugRights;
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(queryErr)),
                            attachBlockText(impl_->block));
    }
    const std::string name = processName(query);
    const bool bit = sameBitness(query);
    WA_LOG(wa::log::Level::Debug, "Attach", "IsWow64Process", name, bit ? "same-bitness" : "cross-bitness");
    CloseHandle(query);

    HANDLE inject = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
                                    PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
                                FALSE, pid);
    const DWORD injOpenErr = inject ? ERROR_SUCCESS : GetLastError();
    const bool hasDebug = inject != nullptr;
    WA_LOG(wa::log::Level::Debug, "Attach", "OpenProcess(inject)", "pid=" + std::to_string(pid),
           wa::log::hrName(HRESULT_FROM_WIN32(injOpenErr)));

    impl_->block = evaluateAttach(pid, ourPid, bit, hasDebug, name);
    if (impl_->block != AttachBlock::None) {
        if (inject) CloseHandle(inject);
        WA_LOG(wa::log::Level::Info, "Attach", "start", "pid=" + std::to_string(pid) + " " + name,
               attachBlockText(impl_->block));
        return Result::Fail(static_cast<long>(E_ACCESSDENIED), attachBlockText(impl_->block));
    }

    wchar_t mapNm[64] = {};
    hook_ipc::mapName(pid, mapNm, 64);
    SetLastError(ERROR_SUCCESS);
    bool sdApplied = false;
    HANDLE map = hook_ipc::createHookMapping(pid, &sdApplied);
    const DWORD mapErr = GetLastError();
    if (!sdApplied) {
        WA_LOG(wa::log::Level::Warn, "Attach", "ConvertStringSecurityDescriptor",
               wa::narrowAscii(mapNm), "ignored");
    }
    WA_LOG(wa::log::Level::Debug, "Attach", "CreateFileMapping", wa::narrowAscii(mapNm),
           map ? (mapErr == ERROR_ALREADY_EXISTS ? "already-exists" : "S_OK")
               : wa::log::hrName(HRESULT_FROM_WIN32(mapErr)));
    if (!map) {
        CloseHandle(inject);
        return win32Result(mapErr, "Attach: CreateFileMapping");
    }
    auto* ring = static_cast<hook_ipc::Ring*>(
        MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook_ipc::Ring)));
    WA_LOG(wa::log::Level::Debug, "Attach", "MapViewOfFile", wa::narrowAscii(mapNm),
           ring ? "S_OK" : wa::log::hrName(HRESULT_FROM_WIN32(GetLastError())));
    if (!ring) {
        const DWORD err = GetLastError();
        CloseHandle(map);
        CloseHandle(inject);
        return win32Result(err, "Attach: MapViewOfFile");
    }

    const bool reuse = mapErr == ERROR_ALREADY_EXISTS && ring->magic == hook_ipc::kMagic &&
                       ring->installed == hook_ipc::kInstallOk;
    if (!reuse) {
        ZeroMemory(ring, sizeof(*ring));
        ring->magic = hook_ipc::kMagic;
        ring->cap = hook_ipc::kCap;
        ring->installed = hook_ipc::kInstallPending;
        const std::wstring wid = utf8ToWide(deviceIdUtf8);
        if (!wid.empty())
            wcsncpy_s(ring->deviceId, wid.c_str(), _TRUNCATE);
        ring->flow = flow == PipelineFlow::Render ? 1u : 0u;
    }

    const std::wstring dll = stageHookDll();
    Result inj = injectDll(inject, dll);
    CloseHandle(inject);
    if (!inj) {
        WA_LOG(wa::log::Level::Info, "Attach", "inject", inj.message, "fail");
        UnmapViewOfFile(ring);
        CloseHandle(map);
        return inj;
    }

    if (!reuse) {
        for (int i = 0; i < 80 && ring->installed == hook_ipc::kInstallPending; ++i)
            Sleep(50);
        const uint32_t installed = ring->installed;
        WA_LOG(installed == hook_ipc::kInstallOk ? wa::log::Level::Debug : wa::log::Level::Info,
               "Attach", "hook installed", "pid=" + std::to_string(pid),
               installed == hook_ipc::kInstallOk ? "S_OK" : attachInstallFailMessage(installed));
        if (installed != hook_ipc::kInstallOk) {
            UnmapViewOfFile(ring);
            CloseHandle(map);
            return Result::Fail(static_cast<long>(E_FAIL), attachInstallFailMessage(installed));
        }
    }

    impl_->map = map;
    impl_->ring = ring;
    impl_->pid = pid;
    impl_->readIndex = ring->writeIndex;
    impl_->block = AttachBlock::None;
    WA_LOG(wa::log::Level::Info, "Attach", "start",
           "pid=" + std::to_string(pid) + " " + name, "attached");
    return Result::Ok();
}

std::vector<HookedCall> OnDemandAttach::drain() {
    std::vector<HookedCall> out;
    if (!attached()) return out;
    const uint32_t write = impl_->ring->writeIndex;
    uint32_t read = impl_->readIndex;
    if (write < read) return out;
    uint32_t count = write - read;
    if (count > hook_ipc::kCap) {
        read = write - hook_ipc::kCap;
        count = hook_ipc::kCap;
    }
    out.reserve(count);
    for (uint32_t i = read; i < write; ++i) {
        const auto& pod = impl_->ring->slots[i % hook_ipc::kCap];
        out.push_back(fromPod(pod));
    }
    impl_->readIndex = write;
    return out;
}

std::vector<HookedCall> OnDemandAttach::pumpRing() const {
    std::vector<HookedCall> out;
    if (!attached() || !impl_->ring->pumpEnabled) return out;
    const uint32_t write = impl_->ring->pumpWriteIndex;
    const uint32_t n = write < hook_ipc::kPumpCap ? write : hook_ipc::kPumpCap;
    out.reserve(n);
    for (uint32_t i = write - n; i < write; ++i)
        out.push_back(fromPod(impl_->ring->pumpSlots[i % hook_ipc::kPumpCap]));
    return out;
}

uint32_t OnDemandAttach::pumpXruns() const {
    if (!attached()) return 0;
    return impl_->ring->pumpXruns;
}

void OnDemandAttach::setPumpEnabled(bool on) {
    if (!impl_ || !impl_->ring) return;
    impl_->ring->pumpEnabled = on ? 1u : 0u;
    WA_LOG(wa::log::Level::Debug, "Attach", "setPumpEnabled",
           on ? "on" : "off", "S_OK");
}

}  // namespace wa
