#pragma once
#include "OnDemandAttach.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wa {

// Owns one OnDemandAttach per PID. Attach adds without detaching others.
class PipelineAttachSet {
public:
    Result start(uint32_t pid, const std::string& deviceIdUtf8 = {},
                 PipelineFlow flow = PipelineFlow::Capture);
    void stop(uint32_t pid);
    void stopAll();

    bool attached(uint32_t pid) const;
    bool anyAttached() const;
    std::vector<uint32_t> attachedPids() const;  // sorted ascending

    std::vector<HookedCall> drainAll();
    uint64_t droppedControlCalls() const;
    std::vector<HookedCall> pumpRingAll() const;
    uint32_t pumpXrunsSum() const;

    void setPumpEnabled(bool on);
    void setCaptureEnabled(bool on);

private:
    std::unordered_map<uint32_t, std::unique_ptr<OnDemandAttach>> byPid_;
};

}  // namespace wa
