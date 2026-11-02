#pragma once
#include "HookedCallPod.h"
#include "HookTransportStatus.h"
#include <array>

namespace wa {

// Receiver-owned cursors and baselines. A poll examines a fixed number of
// cells, independently for every PID; no source can extend another's budget.
class HookTransportReader {
public:
    static constexpr uint32_t kControlBudget = 256;
    void begin(const hook_ipc::Ring& ring, bool pump, uint32_t generation) {
        pump_ = pump;
        generation_ = generation;
        cursor_ = 0;
        epoch_ = hook_ipc::atomicRead64(ring.transportEpoch);
        const auto& counters = pump ? ring.pumpCounters : ring.controlCounters;
        boundary_ = hook_ipc::atomicRead64(counters.reserved);
        overwritten_ = lossCount(counters.overwritten);
        rejected_ = lossCount(counters.rejected);
        lastOverwritten_ = overwritten_;
        lastRejected_ = rejected_;
        unknown_ = false;
    }
    template <typename Receive>
    void poll(hook_ipc::Ring& ring, Receive&& receive, bool finish = false) {
        const uint32_t cap = pump_ ? hook_ipc::kPumpCap : hook_ipc::kCap;
        const uint32_t budget = pump_ || finish ? cap : kControlBudget;
        for (uint32_t scanned = 0; scanned < budget; ++scanned) {
            const uint32_t index = cursor_++ % cap;
            hook_ipc::CallPod pod{};
            uint64_t sequence = 0;
            if (hook_ipc::readCall(ring, pump_, index, generation_, pod, sequence) &&
                sequence - boundary_ < (uint64_t{1} << 63))
                receive(pod, sequence);
        }
    }
    HookTransportLaneStatus snapshot(const hook_ipc::Ring& ring) const {
        const auto& counters = pump_ ? ring.pumpCounters : ring.controlCounters;
        const auto overwritten = lossCount(counters.overwritten);
        const auto rejected = lossCount(counters.rejected);
        // A decrease may be a wrap or an explicit reset. The ABI epoch makes
        // explicit reset detectable; conservatively retain unknown on either.
        unknown_ = unknown_ || overwritten < lastOverwritten_ || rejected < lastRejected_ ||
            hook_ipc::atomicRead64(ring.transportEpoch) != epoch_ ||
            hook_ipc::atomicRead64(counters.unknown) != 0;
        lastOverwritten_ = overwritten;
        lastRejected_ = rejected;
        HookTransportLaneStatus out;
        out.overwritten = static_cast<uint32_t>(overwritten - overwritten_);
        out.writeRejected = static_cast<uint32_t>(rejected - rejected_);
        out.unknown = unknown_;
        const uint32_t cap = pump_ ? hook_ipc::kPumpCap : hook_ipc::kCap;
        for (uint32_t index = 0; index < cap; ++index) {
            const auto& cell = pump_ ? ring.pumpPublication[index] : ring.controlPublication[index];
            const auto owner = hook_ipc::atomicRead64(cell.ownership);
            if (owner == hook_ipc::slotOwner(generation_, hook_ipc::kSlotWriting)) ++out.pending;
        }
        return out;
    }
private:
    uint32_t lossCount(const uint64_t& counter) const {
        const auto tagged = hook_ipc::atomicRead64(counter);
        return static_cast<uint32_t>(tagged >> 32) == generation_ ? static_cast<uint32_t>(tagged) : 0;
    }
    bool pump_ = false;
    uint32_t generation_ = 0, cursor_ = 0;
    uint64_t epoch_ = 0, overwritten_ = 0, rejected_ = 0, boundary_ = 0;
    mutable uint64_t lastOverwritten_ = 0, lastRejected_ = 0;
    mutable bool unknown_ = true;
};
}  // namespace wa
