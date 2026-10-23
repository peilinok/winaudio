#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "PipelineGraph.h"
#include "Result.h"

namespace wa {

// One process visible to the current user (same Windows session and token user).
struct UserProcessInfo {
    uint32_t pid = 0;
    std::string name;  // UTF-8 image name, e.g. "chrome.exe"
};

// Snapshot processes with Toolhelp32. Keeps processes in the caller's session whose
// token user equals the caller's. Processes that cannot be opened or queried are
// skipped silently (protected, elevated or already exited).
Result enumerateUserProcesses(std::vector<UserProcessInfo>& out);

// One process row in the Pipeline Live sessions tree.
struct ProcessTreeRow {
    uint32_t pid = 0;
    std::string name;
    std::vector<int> sessions;  // indices into the Live session list
    bool hasActiveSession = false;
};

// Same image name collapsed under one expandable parent when there are 2+ PIDs.
struct ProcessNameGroup {
    std::string name;
    std::vector<ProcessTreeRow> processes;
    bool hasAudio = false;
    bool hasActiveSession = false;
    int sessionCount = 0;
};

// Group Live sessions by PID and merge them with the user process list.
// audioOnly keeps only PIDs that own at least one Live session. Session PIDs missing
// from processes are still listed. pid 0 and hidePid (when non-zero) are dropped.
// Order: processes with audio sessions first, then case-insensitive name, then pid.
std::vector<ProcessTreeRow> buildProcessTree(const std::vector<UserProcessInfo>& processes,
                                             const std::vector<LiveSessionView>& sessions,
                                             bool audioOnly, uint32_t hidePid);

// Collapse consecutive same-name (case-insensitive) process rows into name groups.
// Singletons stay as a one-process group. Group order: any audio first, then name.
std::vector<ProcessNameGroup> groupProcessTreeByName(std::vector<ProcessTreeRow> rows);

}  // namespace wa
