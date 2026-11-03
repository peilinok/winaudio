#pragma once
#include "ProcessInstance.h"
#include "MethodSelection.h"
#include <array>
#include <cstdint>

namespace wa {

struct MethodCallStatistics {
    uint64_t completed = 0, failed = 0, discontinuities = 0;
    bool unknown = false;
};
struct MethodStatisticsSnapshot {
    bool known = false;
    std::array<MethodCallStatistics, static_cast<size_t>(MonitorMethod::Count)> methods{};
};

struct HookTransportLaneStatus {
    uint64_t overwritten = 0;
    uint64_t writeRejected = 0;
    uint64_t pending = 0;
    bool unknown = true;
};

// Source counts are confirmed per-segment lower bounds in records. Pending is
// a current gauge, not cumulative loss. The receiver owns snapshot/queue counts.
struct HookTransportStatus {
    int64_t snapshotMs = 0;
    // Receiver order is independent of wall-clock adjustments.
    uint64_t snapshotSequence = 0;
    uint32_t observationGeneration = 0;
    HookTransportLaneStatus control, pump;
    uint64_t controllerDroppedControl = 0;
    uint64_t controllerDroppedPump = 0;
    MethodStatisticsSnapshot statistics;
};

struct TransportUpdate {
    ProcessInstance instance;
    uint64_t token = 0;
    HookTransportStatus status;
};

}  // namespace wa
