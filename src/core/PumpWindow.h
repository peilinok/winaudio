#pragma once
#include "HookedCall.h"
#include "HookedCallPod.h"
#include <array>

namespace wa {

// Local observation identities, not a remote publication guarantee.
// The producer reserves pumpWriteIndex before writing its slot.
class PumpWindow {
public:
    void begin(const hook_ipc::Ring& ring);
    const std::vector<PumpCall>& poll(const hook_ipc::Ring& ring);
private:
    struct Slot {
        bool valid = false;
        uint32_t logical = 0;
        uint64_t sequence = 0;
        hook_ipc::CallPod pod{};
        HookedCall call;
    };
    uint32_t boundary_ = 0, lastWrite_ = 0;
    uint64_t nextSequence_ = 0;
    std::array<hook_ipc::CallPod, hook_ipc::kPumpCap> initial_{};
    std::array<Slot, hook_ipc::kPumpCap> slots_{};
    std::vector<PumpCall> current_;
};

HookedCall decodeHookedCallPod(const hook_ipc::CallPod& pod);

}  // namespace wa
