#pragma once
#include "HookedCall.h"
#include "HookedCallPod.h"
#include "HookTransportReader.h"
#include <array>
#include <utility>

namespace wa {

// Local observation identities refer only to completely received records.
class PumpWindow {
public:
    void begin(const hook_ipc::Ring& ring);
    void begin(const hook_ipc::Ring& ring, uint32_t generation);
    const std::vector<PumpCall>& poll(hook_ipc::Ring& ring);
    HookTransportLaneStatus transportStatus(const hook_ipc::Ring& ring) const { return reader_.snapshot(ring); }
    std::vector<PumpCall> takeReceived();
private:
    uint64_t nextSequence_ = 0;
    HookTransportReader reader_;
    std::vector<PumpCall> current_;
    std::vector<PumpCall> received_;
    std::vector<std::pair<uint64_t, HookedCall>> batch_;
};

HookedCall decodeHookedCallPod(const hook_ipc::CallPod& pod);

}  // namespace wa
