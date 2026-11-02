#pragma once
#include "PipelineAttachSet.h"
#include "HookTransportStatus.h"
#include <memory>

namespace wa {

// GUI command boundary. Active objects belong exclusively to the receiver.
class PipelineAttachController {
public:
    static constexpr size_t kQueueCapacity = 32;
    static constexpr size_t kTargetCapacity = 64;
    enum class EventKind { Attached, Failed, Detached, Configured, ConfigurationFailed };
    struct Event {
        uint32_t pid = 0;
        EventKind kind = EventKind::Failed;
        Result result;
        ProcessInstance instance;
        uint64_t token = 0;
        std::string name;
        int64_t snapshotMs = 0;
        bool exited = false;
        uint32_t methods = kDefaultMonitorMethods;
    };
    struct Batch {
        std::vector<Event> events;
        std::vector<HookedCall> calls;
        std::vector<std::pair<uint64_t, std::vector<PumpCall>>> pump; // Receiver segment token.
        std::vector<TransportUpdate> transport;
    };
    struct Snapshot {
        std::vector<uint32_t> attached;
        uint32_t pendingPid = 0, cleanupPending = 0;
        uint64_t droppedCalls = 0, droppedBatches = 0, rejectedCommands = 0;
        uint64_t sourceDroppedCalls = 0;
        uint32_t pumpXruns = 0;
    };
    PipelineAttachController();
    ~PipelineAttachController();
    PipelineAttachController(const PipelineAttachController&) = delete;
    PipelineAttachController& operator=(const PipelineAttachController&) = delete;
    Result start(uint32_t pid, const std::string& deviceId = {},
                 PipelineFlow flow = PipelineFlow::Capture, ProcessInstance expected = {},
                 uint32_t methods = UINT32_MAX);
    Result configure(ProcessInstance instance, uint64_t token, uint32_t methods);
    uint32_t lastMethods(ProcessInstance instance) const;
    bool stop(uint32_t pid);
    bool stopAll();
    void shutdown();  // Cancel and join before frontend logging/global teardown.
    std::vector<Batch> poll();
    Snapshot snapshot() const;
    bool attached(uint32_t pid) const;
    bool anyAttached() const;
    std::vector<uint32_t> attachedPids() const;
    uint64_t droppedControlCalls() const;
    uint32_t pumpXrunsSum() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
