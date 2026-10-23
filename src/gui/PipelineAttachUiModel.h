#pragma once
#include <cstdio>
#include <string>
#include <unordered_map>
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

inline bool canAttach(uint32_t pid, bool currentlyAttached) {
    return pid != 0 && !currentlyAttached;
}

// Status for the typed PID, including when that process is not in the Live session list.
inline PipelineAttachUiState statusForPid(uint32_t pid, bool attached, uint32_t attachedPid,
                                          uint32_t failedPid, bool hasFailBanner) {
    if (pid == 0) return PipelineAttachUiState::Detached;
    if (attached && attachedPid == pid) return PipelineAttachUiState::Attached;
    if (hasFailBanner && failedPid == pid) return PipelineAttachUiState::Failed;
    return PipelineAttachUiState::Detached;
}

// Hook column for a Live session row: Attached/Failed follow the attach PID, not only
// the session-instance key (so PID-only attach still paints matching rows).
inline PipelineAttachUiState hookStatusForRow(
    const LiveSessionView& row, bool attached, uint32_t attachedPid,
    uint32_t failedPid, bool hasFailBanner,
    const std::unordered_map<std::string, PipelineAttachUiState>& uiBySessionKey) {
    if (attached && attachedPid != 0 && attachedPid == row.processId)
        return PipelineAttachUiState::Attached;
    if (hasFailBanner && failedPid != 0 && failedPid == row.processId)
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

}  // namespace wa::pipeline_attach_ui
