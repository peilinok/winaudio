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

HookedCall fromPod(const hook_ipc::CallPod& p) {
    HookedCall c;
    c.streamId = p.streamId;
    c.timeMs = p.timeMs;
    c.iface = p.iface;
    c.method = p.method;
    c.args = p.args;
    c.hresult = p.hresult;
    c.pump = p.pump != 0;
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
    WA_LOG(wait == WAIT_OBJECT_0 ? wa::log::Level::Debug : wa::log::Level::Warn, "Attach",
           "WaitForSingleObject(inject)", "",
           wait == WAIT_OBJECT_0 ? "S_OK" : wa::log::hrName(HRESULT_FROM_WIN32(wait)));
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    if (wait != WAIT_OBJECT_0)
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)),
                            "Attach: inject timed out");
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

Result OnDemandAttach::start(uint32_t pid) {
    stop();
    impl_->block = AttachBlock::PidZero;
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

    const std::wstring dll = hookDllPath();
    Result inj = injectDll(inject, dll);
    CloseHandle(inject);
    if (!inj) {
        WA_LOG(wa::log::Level::Info, "Attach", "inject", inj.message, "fail");
        return inj;
    }

    wchar_t mapNm[64] = {};
    hook_ipc::mapName(pid, mapNm, 64);
    HANDLE map = nullptr;
    DWORD mapErr = ERROR_SUCCESS;
    for (int i = 0; i < 40 && !map; ++i) {
        map = OpenFileMappingW(FILE_MAP_READ, FALSE, mapNm);
        mapErr = map ? ERROR_SUCCESS : GetLastError();
        if (!map) Sleep(50);
    }
    WA_LOG(wa::log::Level::Debug, "Attach", "OpenFileMapping", wa::narrowAscii(mapNm),
           wa::log::hrName(HRESULT_FROM_WIN32(mapErr)));
    if (!map) {
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(mapErr)),
                            "Attach: hook mapping not ready");
    }
    auto* ring = static_cast<hook_ipc::Ring*>(
        MapViewOfFile(map, FILE_MAP_READ, 0, 0, sizeof(hook_ipc::Ring)));
    WA_LOG(wa::log::Level::Debug, "Attach", "MapViewOfFile", wa::narrowAscii(mapNm),
           ring ? "S_OK" : wa::log::hrName(HRESULT_FROM_WIN32(GetLastError())));
    if (!ring || ring->magic != hook_ipc::kMagic) {
        if (ring) UnmapViewOfFile(ring);
        CloseHandle(map);
        return Result::Fail(static_cast<long>(E_FAIL), "Attach: hook mapping invalid");
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

}  // namespace wa
