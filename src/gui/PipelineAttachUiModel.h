#pragma once
#include <cstdio>
#include <ctime>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "AudioSessionEnumerator.h"
#include "CallLogStore.h"
#include "PipelineGraph.h"
#include "PipelineAttachController.h"

namespace wa::pipeline_attach_ui {

struct AttachRequest {
    uint32_t pid = 0;
    std::string deviceId;
    PipelineFlow flow = PipelineFlow::Capture;
    std::string sessionKey;
    ProcessInstance instance;
};

struct MethodDraft {
    uint32_t methods = kDefaultMonitorMethods;
    bool pending = false;
    std::string failure;
    bool dirty(uint32_t published) const { return methods != published; }
};

struct ClearState {
    bool pending = false;
    std::string failure;
};

// Only presentation handoff is paced. The controller keeps receiving all PIDs.
class PresentationRefresh {
public:
    bool take(uint64_t steadyMilliseconds) {
        if (started_ && steadyMilliseconds - previous_ < 100) return false;
        previous_ = steadyMilliseconds;
        started_ = true;
        return true;
    }
private:
    uint64_t previous_ = 0;
    bool started_ = false;
};

inline std::string segmentTime(int64_t milliseconds) {
    const auto seconds = static_cast<std::time_t>(milliseconds / 1000);
    std::tm local{};
    if (localtime_s(&local, &seconds) != 0) return "?";
    char text[32]{};
    std::strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", &local);
    return text;
}

// Selection is presentation state; the Store is the only segment/history owner.
class SegmentSelection {
public:
    uint64_t selected() const { return selected_; }
    bool select(const CallLogStore& store, uint64_t id) {
        if (!store.segment(id)) return false;
        selected_ = id;
        return true;
    }
    template<class Receive>
    CallLogExport freezeExport(CallLogStore& store, int64_t frozenMs, Receive&& receive) {
        const auto selected = selected_;
        receive();
        return store.freezeExport(selected, frozenMs);
    }
    void apply(CallLogStore& store, PipelineAttachController::Batch batch) {
        for (const auto& event : batch.events) {
            if (event.kind != PipelineAttachController::EventKind::Cleared || !event.result) continue;
            const auto id = store.attachForToken(event.token);
            if (const auto* segment = store.segment(id))
                if (segment->instance == event.instance)
                    store.clearSegment(id, event.observationGeneration, event.snapshotMs);
        }
        for (const auto& event : batch.events) {
            if (event.kind != PipelineAttachController::EventKind::Configured) continue;
            const auto id = store.attachForToken(event.token);
            if (const auto* segment = store.segment(id))
                if (segment->instance == event.instance)
                    store.setMethods(id, event.methods, event.snapshotMs, event.observationGeneration);
        }
        // Finish already-known sources before admitting replacement instances.
        // One batch can carry the old source's bounded tail and a new success.
        for (const auto& event : batch.events) {
            if (event.kind != PipelineAttachController::EventKind::Detached) continue;
            const auto id = store.attachForToken(event.token);
            if (!id) continue;
            for (auto& call : batch.calls)
                if (call.attachToken == event.token) store.append(id, std::move(call));
            for (const auto& item : batch.pump)
                if (item.first == event.token) store.appendPump(id, item.second);
            for (const auto& update : batch.transport)
                if (update.token == event.token) store.updateTransport(update);
            store.endAttach(id, event.exited, event.snapshotMs);
        }
        for (const auto& event : batch.events) {
            if (event.kind != PipelineAttachController::EventKind::Attached) continue;
            const auto id = store.registerAttach(event.instance, event.name, event.token, event.snapshotMs, event.methods);
            store.setObservationGeneration(id, event.observationGeneration);
            if (!selected_) selected_ = id;
        }
        for (auto& call : batch.calls) {
            // Output ownership includes the source token. Never resolve by a
            // PID whose active segment may have changed while this was queued.
            const auto id = store.attachForToken(call.attachToken);
            store.append(id, std::move(call));
        }
        for (const auto& item : batch.pump)
            store.appendPump(store.attachForToken(item.first), item.second);
        for (const auto& update : batch.transport) store.updateTransport(update);
        for (const auto& event : batch.events) {
            if (event.kind != PipelineAttachController::EventKind::Detached) continue;
            store.endAttach(store.attachForToken(event.token), event.exited, event.snapshotMs);
        }
    }
private:
    uint64_t selected_ = 0;
};

// Ready means the export was reaped, not merely that its worker signalled done.
class ExportExitState {
public:
    void request() { if (state_ == State::None) state_ = State::Prompt; }
    void choose(bool cancel) { state_ = cancel ? State::Cancel : State::Wait; }
    bool requested() const { return state_ != State::None; }
    bool prompt() const { return state_ == State::Prompt; }
    bool cancelRequested() const { return state_ == State::Cancel; }
    bool ready(bool busy) const { return requested() && !busy; }
private:
    enum class State { None, Prompt, Wait, Cancel };
    State state_ = State::None;
};

// Sync the selected Live session's PID into the Attach PID text box.
inline bool copyLiveSessionPidToBuffer(const std::vector<LiveSessionView>& rows, int index,
                                       char* dst, size_t dstSize) {
    if (!dst || dstSize == 0 || index < 0 || index >= static_cast<int>(rows.size()))
        return false;
    std::snprintf(dst, dstSize, "%u", rows[static_cast<size_t>(index)].processId);
    return true;
}

// Parse the Attach PID box. Reuses Application Loopback rules (nonzero decimal).
inline bool parseAttachPid(const char* text, uint32_t& pidOut) {
    return parseApplicationLoopbackPid(text, pidOut);
}

// Attach is allowed when the typed PID is nonzero and not already hooked.
inline bool canAttach(uint32_t pid, bool thisPidAlreadyAttached) {
    return pid != 0 && !thisPidAlreadyAttached;
}

// Detach (single) is enabled when the typed PID is attached or has a fail banner.
inline bool canDetachPid(uint32_t pid, const std::unordered_set<uint32_t>& attached,
                         const std::unordered_map<uint32_t, std::string>& failByPid) {
    if (pid == 0) return false;
    if (attached.find(pid) != attached.end()) return true;
    return failByPid.find(pid) != failByPid.end();
}

inline bool canDetachAll(const std::unordered_set<uint32_t>& attached,
                         const std::unordered_map<uint32_t, std::string>& failByPid) {
    return !attached.empty() || !failByPid.empty();
}

// Status for a process PID against the multi-attach set and per-PID fail map.
inline PipelineAttachUiState statusForPid(uint32_t pid,
                                          const std::unordered_set<uint32_t>& attached,
                                          const std::unordered_map<uint32_t, std::string>& failByPid) {
    if (pid == 0) return PipelineAttachUiState::Detached;
    if (attached.find(pid) != attached.end()) return PipelineAttachUiState::Attached;
    if (failByPid.find(pid) != failByPid.end()) return PipelineAttachUiState::Failed;
    return PipelineAttachUiState::Detached;
}

// Hook column for a Live session row: Attached/Failed follow the process PID.
inline PipelineAttachUiState hookStatusForRow(
    const LiveSessionView& row, const std::unordered_set<uint32_t>& attached,
    const std::unordered_map<uint32_t, std::string>& failByPid,
    const std::unordered_map<std::string, PipelineAttachUiState>& uiBySessionKey) {
    if (attached.find(row.processId) != attached.end())
        return PipelineAttachUiState::Attached;
    if (failByPid.find(row.processId) != failByPid.end())
        return PipelineAttachUiState::Failed;
    const std::string key = liveSessionAttachKey(row);
    const auto it = uiBySessionKey.find(key);
    if (it != uiBySessionKey.end()) return it->second;
    return PipelineAttachUiState::Detached;
}

// Prefer deviceId/flow from a selected Live session that shares the attach PID.
inline bool sessionHintForPid(const std::vector<LiveSessionView>& rows, int selected,
                              uint32_t pid, std::string& deviceIdOut, PipelineFlow& flowOut,
                              std::string& sessionKeyOut) {
    deviceIdOut.clear();
    sessionKeyOut.clear();
    flowOut = PipelineFlow::Capture;
    if (pid == 0 || selected < 0 || selected >= static_cast<int>(rows.size())) return false;
    const LiveSessionView& row = rows[static_cast<size_t>(selected)];
    if (row.processId != pid) return false;
    deviceIdOut = row.deviceId;
    flowOut = row.flow;
    sessionKeyOut = liveSessionAttachKey(row);
    return true;
}

inline std::string formatAttachedPids(const std::vector<uint32_t>& pids) {
    if (pids.empty()) return {};
    std::string out;
    for (size_t i = 0; i < pids.size(); ++i) {
        if (i) out += ", ";
        out += std::to_string(pids[i]);
    }
    return out;
}

}  // namespace wa::pipeline_attach_ui
