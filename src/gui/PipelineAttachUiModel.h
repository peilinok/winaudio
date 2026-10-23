#pragma once
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "AudioSessionEnumerator.h"
#include "CallLogStore.h"
#include "PipelineGraph.h"

namespace wa::pipeline_attach_ui {

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
