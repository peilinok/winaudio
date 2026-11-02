#pragma once
#include "OnDemandAttach.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wa {

class CallLogStore;

// Owns one OnDemandAttach per PID. Attach adds without detaching others.
class PipelineAttachSet {
public:
    Result start(uint32_t pid, const std::string& deviceIdUtf8 = {},
                 PipelineFlow flow = PipelineFlow::Capture);
    void stop(uint32_t pid);
    void stopAll();
    Result adopt(std::unique_ptr<OnDemandAttach> attach);
    std::vector<uint32_t> exitedPids() const;
    std::vector<std::pair<uint32_t, std::vector<PumpCall>>> pumpWindows() const;

    bool attached(uint32_t pid) const;
    bool anyAttached() const;
    std::vector<uint32_t> attachedPids() const;  // sorted ascending
    ProcessInstance instance(uint32_t pid) const;
    std::string processName(uint32_t pid) const;
    std::vector<HookedCall> finish(uint32_t pid);

    std::vector<HookedCall> drainAll();
    uint64_t droppedControlCalls() const;
    std::vector<HookedCall> pumpRingAll() const;
    void updatePump(CallLogStore& store) const;
    uint32_t pumpXrunsSum() const;

    void setPumpEnabled(bool on);
    void setCaptureEnabled(bool on);

private:
    std::unordered_map<uint32_t, std::unique_ptr<OnDemandAttach>> byPid_;
};

}  // namespace wa
