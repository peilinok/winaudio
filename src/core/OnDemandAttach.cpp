#include "OnDemandAttach.h"
#include "RemoteInjection.h"
#include "HookModuleIdentity.h"
#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "PumpWindow.h"
#include "HookedCall.h"
#include "HookedCallPod.h"
#include "Log.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <cstring>
#include <algorithm>
#include <mutex>
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

Result stageHookDll(std::wstring& staged) {
    const std::wstring source = hookDllPath();
    const DWORD attributes = GetFileAttributesW(source.c_str());
    const DWORD error = attributes == INVALID_FILE_ATTRIBUTES ? GetLastError() : ERROR_SUCCESS;
    WA_LOG(wa::log::Level::Debug, "Attach", "GetFileAttributesW(hook)", wa::narrowAscii(source),
           wa::log::hrName(HRESULT_FROM_WIN32(error)));
    if (error != ERROR_SUCCESS)
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(error)),
                            "Attach: cannot verify hook file identity");
    return stageHookModule(source, staged);
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

Result win32Result(DWORD err, const char* where) {
    if (err == ERROR_SUCCESS) return Result::Ok();
    return HrToResult(HRESULT_FROM_WIN32(err), where);
}

Result verifyLoadedModule(uint32_t pid, const std::wstring& dll, bool& definitelyAbsent);

struct AttachResources {
    HANDLE process = nullptr;
    uint32_t pid = 0;
    FILETIME created{};
    HANDLE map = nullptr;
    hook_ipc::Ring* ring = nullptr;
    RemoteInjection injection;
    bool installationStarted = false;
    bool moduleVerified = false;
    std::wstring expectedDll;

    ~AttachResources() {
        if (ring) {
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&ring->captureEnabled), 0);
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&ring->pumpEnabled), 0);
            if (!finished()) {
                WA_LOG(wa::log::Level::Warn, "Attach", "cleanup unresolved at shutdown",
                       "pid=" + std::to_string(pid), "target left running; remote parameter not reclaimed");
            }
            UnmapViewOfFile(ring);
        }
        if (map) CloseHandle(map);
        if (process) CloseHandle(process);
    }

    bool finished() {
        if (!injection.poll()) return false;
        const auto target = waitRemoteThread(process, 0);
        if (target.outcome == WAIT_FAILED) {
            WA_LOG(wa::log::Level::Warn, "Attach", "WaitForSingleObject(installation cleanup)", "target state unknown",
                   wa::log::hrName(HRESULT_FROM_WIN32(target.error)));
        }
        if (target.outcome == WAIT_OBJECT_0) return true;
        if (installationStarted && !moduleVerified) {
            bool absent = false;
            const Result loaded = verifyLoadedModule(pid, expectedDll, absent);
            if (loaded) moduleVerified = true;
            else if (absent) installationStarted = false;
            else return false;  // Enumeration failure is not proof of absence.
        }
        return !installationStarted || !ring || ring->installed != hook_ipc::kInstallPending;
    }
};

// Entries also reserve an instance during installation. External owners keep an
// active entry alive; orphaned entries are reaped only after both lifetimes end.
struct AttachReservations {
    std::mutex mutex;
    std::vector<std::shared_ptr<AttachResources>> entries;

    void reap() {
        entries.erase(std::remove_if(entries.begin(), entries.end(), [](const auto& entry) {
            return entry.use_count() == 1 && entry->finished();
        }), entries.end());
    }
};

AttachReservations& reservations() {
    static AttachReservations state;
    return state;
}

Result reserveInstance(HANDLE process, uint32_t pid, std::shared_ptr<AttachResources>& out) {
    FILETIME created{}, exit{}, kernel{}, user{};
    const BOOL identified = GetProcessTimes(process, &created, &exit, &kernel, &user);
    const DWORD identityError = identified ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "GetProcessTimes(reserve)", "pid=" + std::to_string(pid),
           wa::log::hrName(HRESULT_FROM_WIN32(identityError)));
    if (!identified) return win32Result(identityError, "Attach: GetProcessTimes");
    auto& state = reservations();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.reap();
    for (const auto& entry : state.entries) {
        if (entry->pid == pid && CompareFileTime(&entry->created, &created) == 0)
            return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)),
                "Attach: this process is already attached or remote cleanup is pending; retry after completion");
    }
    // Admission happens before any remote allocation or thread is created.
    if (state.entries.size() >= 32)
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)),
                            "Attach: remote resource capacity reached; cleanup pending");
    out = std::make_shared<AttachResources>();
    out->pid = pid;
    out->created = created;
    out->process = process;
    state.entries.push_back(out);
    return Result::Ok();
}

void reapReservations() {
    auto& state = reservations();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.reap();
}

Result verifyLoadedModule(uint32_t pid, const std::wstring& dll, bool& definitelyAbsent) {
    definitelyAbsent = false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    const DWORD error = snapshot == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    WA_LOG(wa::log::Level::Debug, "Attach", "CreateToolhelp32Snapshot(modules)",
           "pid=" + std::to_string(pid), wa::log::hrName(HRESULT_FROM_WIN32(error)));
    if (snapshot == INVALID_HANDLE_VALUE) return win32Result(error, "Attach: inspect loaded module");
    MODULEENTRY32W module{};
    module.dwSize = sizeof(module);
    bool found = false;
    for (BOOL have = Module32FirstW(snapshot, &module); have; have = Module32NextW(snapshot, &module)) {
        if (_wcsicmp(module.szExePath, dll.c_str()) == 0) { found = true; break; }
    }
    const DWORD enumerationError = found ? ERROR_SUCCESS : GetLastError();
    CloseHandle(snapshot);
    if (!found && enumerationError != ERROR_NO_MORE_FILES) {
        WA_LOG(wa::log::Level::Warn, "Attach", "Module32First/Next", "coverage unknown",
               wa::log::hrName(HRESULT_FROM_WIN32(enumerationError)));
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(enumerationError)),
                            "Attach: module enumeration failed; cleanup pending");
    }
    definitelyAbsent = !found;
    WA_LOG(found ? wa::log::Level::Debug : wa::log::Level::Warn, "Attach", "verify loaded module",
           wa::narrowAscii(dll), found ? "S_OK" : "not found");
    return found ? Result::Ok()
                 : Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND)),
                                "Attach: LoadLibraryW did not load the expected module");
}

Result injectDll(AttachResources& resources, const std::wstring& dll, const std::atomic_bool* cancelled) {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    auto load = k32 ? GetProcAddress(k32, "LoadLibraryW") : nullptr;
    const DWORD loadError = load ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "GetProcAddress(LoadLibraryW)", "",
           wa::log::hrName(HRESULT_FROM_WIN32(loadError)));
    if (!load) return Result::Fail(static_cast<long>(E_FAIL), "Attach: LoadLibraryW not found");
    resources.expectedDll = dll;
    Result result = resources.injection.begin(resources.process, reinterpret_cast<LPTHREAD_START_ROUTINE>(load),
                                    dll.c_str(), (dll.size() + 1) * sizeof(wchar_t));
    if (!result) return result;
    resources.installationStarted = true;
    result = resources.injection.wait(10000, cancelled);
    if (!result) return result;
    bool absent = false;
    result = verifyLoadedModule(resources.pid, dll, absent);
    resources.moduleVerified = static_cast<bool>(result);
    if (absent)
        resources.installationStarted = false;  // No expected DLL exists to finish installation.
    return result;
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

const char* attachInstallFailMessage(uint32_t installed) {
    return installed == hook_ipc::kInstallFailed
               ? "Attach: hook install failed in target"
               : "Attach: hook install did not finish; restart the target app and retry";
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
    std::shared_ptr<AttachResources> resources;
    uint32_t pid = 0;
    ProcessInstance instance;
    std::string name;
    uint32_t generation = 0;
    AttachBlock block = AttachBlock::None;
    HANDLE map = nullptr;
    hook_ipc::Ring* ring = nullptr;
    uint32_t readIndex = 0;
    uint64_t droppedControl = 0;
    uint32_t reportedRejections = 0;
    PumpWindow pump;
};

OnDemandAttach::OnDemandAttach() : impl_(std::make_unique<Impl>()) {}
OnDemandAttach::~OnDemandAttach() { stop(); }

void OnDemandAttach::stop() {
    if (!impl_) return;
    if (impl_->ring) {
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&impl_->ring->captureEnabled), 0);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&impl_->ring->pumpEnabled), 0);
        impl_->ring = nullptr;
    }
    if (impl_->map) {
        impl_->map = nullptr;
    }
    if (impl_->pid) {
        WA_LOG(wa::log::Level::Info, "Attach", "stop", "pid=" + std::to_string(impl_->pid), "ok");
    }
    impl_->pid = 0;
    impl_->instance = {};
    impl_->name.clear();
    impl_->readIndex = 0;
    impl_->droppedControl = 0;
    impl_->reportedRejections = 0;
    impl_->resources.reset();
    reapReservations();
}

bool OnDemandAttach::attached() const {
    return impl_ && impl_->ring && impl_->pid != 0 && impl_->block == AttachBlock::None;
}

bool OnDemandAttach::targetExited() const {
    if (!attached()) return false;
    const DWORD waited = WaitForSingleObject(impl_->resources->process, 0);
    const DWORD error = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    if (waited == WAIT_FAILED)
        WA_LOG(wa::log::Level::Warn, "Attach", "WaitForSingleObject(active target)",
               "pid=" + std::to_string(impl_->pid), wa::log::hrName(HRESULT_FROM_WIN32(error)));
    return waited == WAIT_OBJECT_0;
}

uint32_t OnDemandAttach::pid() const { return impl_ ? impl_->pid : 0; }
ProcessInstance OnDemandAttach::instance() const { return impl_->instance; }
const std::string& OnDemandAttach::processName() const { return impl_->name; }

Result queryProcessInstance(uint32_t pid, ProcessInstance& instance, std::string* name) {
    instance = {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    const DWORD openError = process ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "OpenProcess(identity)", "pid=" + std::to_string(pid),
           wa::log::hrName(HRESULT_FROM_WIN32(openError)));
    if (!process) return win32Result(openError, "Attach: process identity unavailable");
    FILETIME created{}, exited{}, kernel{}, user{};
    const BOOL ok = GetProcessTimes(process, &created, &exited, &kernel, &user);
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "GetProcessTimes(identity)", "pid=" + std::to_string(pid),
           wa::log::hrName(HRESULT_FROM_WIN32(error)));
    const DWORD waited = WaitForSingleObject(process, 0);
    const DWORD waitError = waited == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    WA_LOG(wa::log::Level::Debug, "Attach", "WaitForSingleObject(identity)", "pid=" + std::to_string(pid),
           waited == WAIT_TIMEOUT ? "running" : wa::log::hrName(HRESULT_FROM_WIN32(waitError)));
    if (ok && waited == WAIT_TIMEOUT) {
        instance = {pid, (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime};
        if (name) *name = processName(process);
    }
    const BOOL closed = CloseHandle(process);
    const DWORD closeError = closed ? ERROR_SUCCESS : GetLastError();
    WA_LOG(closed ? wa::log::Level::Debug : wa::log::Level::Warn, "Attach", "CloseHandle(identity)",
           "pid=" + std::to_string(pid), wa::log::hrName(HRESULT_FROM_WIN32(closeError)));
    if (!ok) return win32Result(error, "Attach: process identity unavailable");
    if (!instance) return win32Result(waited == WAIT_FAILED ? waitError : ERROR_PROCESS_ABORTED,
                                     "Attach: target exited or identity unavailable");
    return Result::Ok();
}

AttachBlock OnDemandAttach::lastBlock() const {
    return impl_ ? impl_->block : AttachBlock::PidZero;
}

uint32_t OnDemandAttach::reapPending() {
    auto& state = reservations();
    std::lock_guard<std::mutex> lock(state.mutex);
    state.reap();
    return static_cast<uint32_t>(std::count_if(state.entries.begin(), state.entries.end(),
        [](const auto& entry) { return entry.use_count() == 1; }));
}

Result OnDemandAttach::start(uint32_t pid, const std::string& deviceIdUtf8, PipelineFlow flow,
                             const std::atomic_bool* cancelled, ProcessInstance expected) {
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
    const std::string name = wa::processName(query);
    const bool bit = sameBitness(query);
    WA_LOG(wa::log::Level::Debug, "Attach", "IsWow64Process", name, bit ? "same-bitness" : "cross-bitness");
    FILETIME queriedCreation{}, exited{}, kernel{}, user{};
    const BOOL queriedIdentity = GetProcessTimes(query, &queriedCreation, &exited, &kernel, &user);
    const DWORD identityError = queriedIdentity ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "GetProcessTimes(query)", "pid=" + std::to_string(pid),
           wa::log::hrName(HRESULT_FROM_WIN32(identityError)));
    CloseHandle(query);
    if (!queriedIdentity) return win32Result(identityError, "Attach: query process identity");
    const ProcessInstance queried{pid, (static_cast<uint64_t>(queriedCreation.dwHighDateTime) << 32) |
                                      queriedCreation.dwLowDateTime};
    if (expected && expected != queried)
        return win32Result(ERROR_PROCESS_ABORTED, "Attach: expected process instance changed");

    HANDLE inject = OpenProcess(SYNCHRONIZE | PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
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

    FILETIME injectedCreation{};
    const BOOL verifiedIdentity = GetProcessTimes(inject, &injectedCreation, &exited, &kernel, &user);
    const DWORD verifiedError = verifiedIdentity ? ERROR_SUCCESS : GetLastError();
    WA_LOG(wa::log::Level::Debug, "Attach", "GetProcessTimes(inject)", "pid=" + std::to_string(pid),
           wa::log::hrName(HRESULT_FROM_WIN32(verifiedError)));
    if (!verifiedIdentity) {
        CloseHandle(inject);
        return win32Result(verifiedError, "Attach: verify process identity");
    }
    if (CompareFileTime(&queriedCreation, &injectedCreation) != 0) {
        CloseHandle(inject);
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED)),
                            "Attach: process identity changed before injection");
    }
    const auto alive = waitRemoteThread(inject, 0);
    WA_LOG(wa::log::Level::Debug, "Attach", "WaitForSingleObject(before injection)", "pid=" + std::to_string(pid),
           alive.outcome == WAIT_TIMEOUT ? "running" : wa::log::hrName(HRESULT_FROM_WIN32(alive.error)));
    if (alive.outcome != WAIT_TIMEOUT) {
        CloseHandle(inject);
        return win32Result(alive.outcome == WAIT_FAILED ? alive.error : ERROR_PROCESS_ABORTED,
                           "Attach: target exited before injection");
    }

    if (cancelled && cancelled->load()) {
        CloseHandle(inject);
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)), "Attach: cancelled before injection");
    }
    std::shared_ptr<AttachResources> resources;
    Result reserved = reserveInstance(inject, pid, resources);
    if (!reserved) { CloseHandle(inject); return reserved; }

    std::wstring dll;
    Result staged = stageHookDll(dll);
    if (!staged) return staged;
    bool moduleLoaded = false;
    Result identity = inspectHookModules(pid, dll, moduleLoaded);
    if (!identity) return identity;

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
        return win32Result(mapErr, "Attach: CreateFileMapping");
    }
    resources->map = map;
    auto* ring = static_cast<hook_ipc::Ring*>(
        MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook_ipc::Ring)));
    WA_LOG(wa::log::Level::Debug, "Attach", "MapViewOfFile", wa::narrowAscii(mapNm),
           ring ? "S_OK" : wa::log::hrName(HRESULT_FROM_WIN32(GetLastError())));
    if (!ring) {
        const DWORD err = GetLastError();
        return win32Result(err, "Attach: MapViewOfFile");
    }
    resources->ring = ring;

    if (moduleLoaded && mapErr == ERROR_ALREADY_EXISTS && ring->magic == hook_ipc::kMagic &&
        ring->installed == hook_ipc::kInstallFailed) {
        if (ReadULongAcquire(reinterpret_cast<const volatile ULONG*>(&ring->coverageFlags)) &
            hook_ipc::kCoverageInstallationFault)
            return Result::Fail(static_cast<long>(E_FAIL),
                "Attach: hook page protection could not be restored; restart the target app");
        return Result::Fail(static_cast<long>(E_FAIL), attachInstallFailMessage(hook_ipc::kInstallFailed));
    }

    const bool reuse = moduleLoaded && mapErr == ERROR_ALREADY_EXISTS && ring->magic == hook_ipc::kMagic &&
                       ring->installed == hook_ipc::kInstallOk;
    if (moduleLoaded && !reuse)
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)),
            "Attach: loaded hook is not ready or its mapping is incompatible; restart the target app and retry");
    if (!reuse) {
        ZeroMemory(ring, sizeof(*ring));
        ring->magic = hook_ipc::kMagic;
        ring->cap = hook_ipc::kCap;
        ring->installed = hook_ipc::kInstallPending;
        ring->processCreated = queried.created;
        const std::wstring wid = utf8ToWide(deviceIdUtf8);
        if (!wid.empty())
            wcsncpy_s(ring->deviceId, wid.c_str(), _TRUNCATE);
        ring->flow = flow == PipelineFlow::Render ? 1u : 0u;
    }

    if (!reuse) {
        Result inj = injectDll(*resources, dll, cancelled);
        if (!inj) {
            WA_LOG(wa::log::Level::Info, "Attach", "inject", inj.message, "fail");
            return inj;
        }
    }

    if (!reuse) {
        for (int i = 0; i < 80 && ring->installed == hook_ipc::kInstallPending; ++i) {
            if (cancelled && cancelled->load())
                return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)),
                                    "Attach: installation cancelled; cleanup pending");
            const DWORD waited = WaitForSingleObject(resources->process, 50);
            if (waited == WAIT_OBJECT_0)
                return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED)), "Attach: target exited");
            if (waited == WAIT_FAILED) {
                const DWORD error = GetLastError();
                WA_LOG(wa::log::Level::Warn, "Attach", "WaitForSingleObject(installation)",
                       "pid=" + std::to_string(pid), wa::log::hrName(HRESULT_FROM_WIN32(error)));
                return win32Result(error, "Attach: installation wait failed; cleanup pending");
            }
        }
    }
    const uint32_t installed = ring->installed;
    WA_LOG(installed == hook_ipc::kInstallOk ? wa::log::Level::Debug : wa::log::Level::Info,
           "Attach", "hook installed", "pid=" + std::to_string(pid),
           installed == hook_ipc::kInstallOk ? "S_OK" : attachInstallFailMessage(installed));
    // A later installation can fault after initial readiness. Apply the same
    // acceptance gate to a newly installed and a reused resident hook.
    const auto coverageFlags = ReadULongAcquire(
        reinterpret_cast<const volatile ULONG*>(&ring->coverageFlags));
    if (coverageFlags & hook_ipc::kCoverageInstallationFault)
        return Result::Fail(static_cast<long>(E_FAIL),
            "Attach: hook page protection could not be restored; restart the target app");
    if (installed != hook_ipc::kInstallOk)
        return Result::Fail(static_cast<long>(E_FAIL), installed == hook_ipc::kInstallPending
            ? "Attach: hook install timed out; cleanup pending; retry after completion"
            : attachInstallFailMessage(installed));

    const auto waited = waitRemoteThread(resources->process, 0);
    WA_LOG(wa::log::Level::Debug, "Attach", "WaitForSingleObject(receive handoff)", "pid=" + std::to_string(pid),
           waited.outcome == WAIT_TIMEOUT ? "running" : wa::log::hrName(HRESULT_FROM_WIN32(waited.error)));
    if (waited.outcome != WAIT_TIMEOUT)
        return win32Result(waited.outcome == WAIT_FAILED ? waited.error : ERROR_PROCESS_ABORTED,
                           "Attach: target exited before receive handoff");
    if (ring->processCreated != queried.created)
        return win32Result(ERROR_PROCESS_ABORTED, "Attach: receiver process identity mismatch");
    if (ring->observationGeneration == UINT32_MAX)
        return win32Result(ERROR_ARITHMETIC_OVERFLOW, "Attach: observation generation exhausted; restart target");

    if (cancelled && cancelled->load())
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)), "Attach: cancelled before capture");
    impl_->resources = std::move(resources);
    impl_->map = map;
    impl_->ring = ring;
    impl_->pid = pid;
    impl_->instance = queried;
    impl_->name = name;
    impl_->generation = static_cast<uint32_t>(InterlockedIncrement(
        reinterpret_cast<volatile LONG*>(&ring->observationGeneration)));
    impl_->readIndex = ring->writeIndex;
    impl_->block = AttachBlock::None;
    impl_->pump.begin(*ring);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&ring->captureEnabled), 1);
    WA_LOG(wa::log::Level::Info, "Attach", "start",
           "pid=" + std::to_string(pid) + " " + name,
           "attached; limited to discovered implementations, not all objects");
    const auto capability = coverage();
    WA_LOG(capability.rejectedAttempts ? wa::log::Level::Warn : wa::log::Level::Info,
           "Attach", "hook coverage", "accepted attempts=" + std::to_string(capability.acceptedAttempts) +
               " rejected attempts=" + std::to_string(capability.rejectedAttempts),
           wa::log::hrName(HRESULT_FROM_WIN32(capability.lastError)));
    return Result::Ok();
}

std::vector<HookedCall> OnDemandAttach::drain() {
    std::vector<HookedCall> out;
    if (!attached()) return out;
    const auto capability = coverage();
    if (capability.rejectedAttempts != impl_->reportedRejections) {
        impl_->reportedRejections = capability.rejectedAttempts;
        WA_LOG(wa::log::Level::Warn, "Attach", capability.restartRequired
                   ? "hook installation fault; target restart required" : "hook coverage incomplete",
               "pid=" + std::to_string(impl_->pid) + " rejected attempts=" +
                   std::to_string(capability.rejectedAttempts),
               wa::log::hrName(HRESULT_FROM_WIN32(capability.lastError)));
    }
    const uint32_t write = impl_->ring->writeIndex;
    uint32_t read = impl_->readIndex;
    if (write < read) return out;
    uint32_t count = write - read;
    if (count > hook_ipc::kCap) {
        impl_->droppedControl += count - hook_ipc::kCap;
        read = write - hook_ipc::kCap;
        count = hook_ipc::kCap;
    }
    out.reserve(count);
    for (uint32_t i = read; i < write; ++i) {
        const auto& pod = impl_->ring->slots[i % hook_ipc::kCap];
        if (pod.observationGeneration != impl_->generation) continue;
        out.push_back(decodeHookedCallPod(pod));
    }
    impl_->readIndex = write;
    return out;
}

uint64_t OnDemandAttach::droppedControlCalls() const {
    return impl_ ? impl_->droppedControl : 0;
}

HookCoverage OnDemandAttach::coverage() const {
    HookCoverage result;
    if (!impl_ || !impl_->ring) return result;
    const auto* ring = impl_->ring;
    result.acceptedAttempts = ReadULongAcquire(reinterpret_cast<const volatile ULONG*>(&ring->patchAccepted));
    result.rejectedAttempts = ReadULongAcquire(reinterpret_cast<const volatile ULONG*>(&ring->patchRejected));
    result.lastError = ReadULongAcquire(reinterpret_cast<const volatile ULONG*>(&ring->patchLastError));
    result.restartRequired = (ReadULongAcquire(
        reinterpret_cast<const volatile ULONG*>(&ring->coverageFlags)) & hook_ipc::kCoverageInstallationFault) != 0;
    return result;
}

std::vector<HookedCall> OnDemandAttach::pumpRing() const {
    std::vector<HookedCall> out;
    for (const auto& observed : pumpWindow()) out.push_back(observed.call);
    return out;
}

const std::vector<PumpCall>& OnDemandAttach::pumpWindow() const {
    static const std::vector<PumpCall> empty;
    return attached() ? impl_->pump.poll(*impl_->ring) : empty;
}

uint32_t OnDemandAttach::pumpXruns() const {
    if (!attached()) return 0;
    return impl_->ring->pumpXruns;
}

void OnDemandAttach::setPumpEnabled(bool on) {
    if (!impl_ || !impl_->ring) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&impl_->ring->pumpEnabled), on ? 1 : 0);
    WA_LOG(wa::log::Level::Debug, "Attach", "setPumpEnabled",
           on ? "on" : "off", "S_OK");
}

void OnDemandAttach::setCaptureEnabled(bool on) {
    if (!impl_ || !impl_->ring) return;
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&impl_->ring->captureEnabled), on ? 1 : 0);
    WA_LOG(wa::log::Level::Debug, "Attach", "setCaptureEnabled",
           on ? "on" : "off", "S_OK");
}

}  // namespace wa

