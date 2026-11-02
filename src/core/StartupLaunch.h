#pragma once
#include "Result.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace wa {

enum class StartupStage : uint8_t {
    Created, EntryHeld, Preparing, ReceiverReady, EntryReleased, EntryAdvanced, DebugDetached, Committed
};
const char* startupStageName(StartupStage stage);

struct StartupRequest {
    std::wstring executable;
    std::wstring arguments;
    std::wstring directory;
    uint32_t timeoutMs = 15000;
};

struct StartupOutcome {
    Result result;
    uint32_t pid = 0;
    uint32_t mainThreadId = 0;
    uint64_t creationTime = 0;
    uintptr_t imageBase = 0;
    uintptr_t entryAddress = 0;
    uint32_t entryRva = 0;
    uint8_t originalByte = 0;
    bool entryRestored = false;
    bool entryAdvanced = false;
    bool debugDetached = false;
    bool committed = false;
    bool targetExitConfirmed = false;
};

// Runs on a dedicated debugger thread and blocks the caller. Preparation runs
// concurrently while the debugger continues events and the EXE entry is held.
// The preparation owner must retain its receiving/attach state after success.
// Preparation must return on cancellation or target exit. A returned operation
// must retain any remote resources still in use, as OnDemandAttach does.
// Progress is synchronous, on the debugger thread; callers must not block it.
// Failure/cancellation terminates only the new process created by this call.
// A single job policy update is the commit point: after it, closing the caller
// no longer terminates the target. EXE-entry coverage excludes TLS/DLL startup.
using StartupPrepare = std::function<Result(uint32_t, const std::atomic_bool&)>;
using StartupProgress = std::function<void(StartupStage, const StartupOutcome&)>;
StartupOutcome launchAtEntry(const StartupRequest& request, const StartupPrepare& prepare,
                             const std::atomic_bool* cancelled = nullptr,
                             const StartupProgress& progress = {});

}  // namespace wa
