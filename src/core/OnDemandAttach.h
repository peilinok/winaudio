#pragma once
#include "HookedCall.h"
#include "PipelineGraph.h"
#include "Result.h"
#include <cstdint>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace wa {

struct PumpCall;

inline constexpr bool kAttachLaunchSuspended = false;
inline constexpr bool kAttachAutoInject = false;

enum class AttachBlock : uint8_t {
    None,
    PidZero,
    SelfProcess,
    Audiodg,
    CrossBitness,
    NoDebugRights,
};

const char* attachBlockText(AttachBlock block);

// Pure policy. Tests use this seam; they do not inject.
AttachBlock evaluateAttach(uint32_t pid, uint32_t ourPid, bool sameBitness, bool hasDebugRights,
                           const std::string& processName);

// Message for a failed hook install. Callers must snapshot `installed` before
// UnmapViewOfFile 鈥?the view is invalid afterwards.
const char* attachInstallFailMessage(uint32_t installed);

std::wstring stagedHookFileName();

class OnDemandAttach {
public:
    OnDemandAttach();
    ~OnDemandAttach();
    OnDemandAttach(const OnDemandAttach&) = delete;
    OnDemandAttach& operator=(const OnDemandAttach&) = delete;

    Result start(uint32_t pid, const std::string& deviceIdUtf8 = {},
                 PipelineFlow flow = PipelineFlow::Capture,
                 const std::atomic_bool* cancelled = nullptr);
    void stop();
    bool attached() const;
    uint32_t pid() const;
    AttachBlock lastBlock() const;
    std::vector<HookedCall> drain();
    uint64_t droppedControlCalls() const;
    std::vector<HookedCall> pumpRing() const;
    const std::vector<PumpCall>& pumpWindow() const;
    uint32_t pumpXruns() const;
    void setPumpEnabled(bool on);
    void setCaptureEnabled(bool on);
    // Nonblocking maintenance; returns the number of unresolved orphaned entries.
    static uint32_t reapPending();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wa

