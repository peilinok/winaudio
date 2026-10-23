#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "PipelineGraph.h"
#include "Result.h"

namespace wa {

struct UserProcessInfo {
    uint32_t pid = 0;
    uint32_t parentPid = 0;  // Toolhelp th32ParentProcessID; 0 if unknown
    std::string name;        // UTF-8 image name, e.g. "chrome.exe"
};

Result enumerateUserProcesses(std::vector<UserProcessInfo>& out);

struct ProcessTreeRow {
    uint32_t pid = 0;
    uint32_t parentPid = 0;
    std::string name;
    std::vector<int> sessions;
    bool hasActiveSession = false;
};

struct ProcessNameGroup {
    std::string name;
    std::vector<ProcessTreeRow> processes;
    bool hasAudio = false;
    bool hasActiveSession = false;
    int sessionCount = 0;
    // PID shown on the name-group root row: Windows-tree root instance among the
    // same-name PIDs (parent outside the group). Prefer Active/audio, then lowest pid.
    // Never 0 when the group is non-empty.
    uint32_t rootPid = 0;
};

std::vector<ProcessTreeRow> buildProcessTree(const std::vector<UserProcessInfo>& processes,
                                             const std::vector<LiveSessionView>& sessions,
                                             bool audioOnly, uint32_t hidePid);

std::vector<ProcessNameGroup> groupProcessTreeByName(std::vector<ProcessTreeRow> rows);

// Pick the name-group root PID (see ProcessNameGroup::rootPid).
uint32_t pickNameGroupRootPid(const std::vector<ProcessTreeRow>& processes);

// col: 0 = name, 1 = PID (rootPid / process pid), 5 = session state (Active first).
// Other col values fall back to name. asc = true for ascending.
void sortProcessNameGroups(std::vector<ProcessNameGroup>& groups, int col, bool ascending);

}  // namespace wa