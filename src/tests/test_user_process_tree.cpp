#include <gtest/gtest.h>
#include <windows.h>
#include <algorithm>
#include "UserProcessEnumerator.h"

namespace {

wa::LiveSessionView session(uint32_t pid, const char* name, const char* state) {
    wa::LiveSessionView s;
    s.processId = pid;
    s.processName = name;
    s.state = state;
    return s;
}

wa::UserProcessInfo proc(uint32_t pid, const char* name, uint32_t parentPid = 0) {
    return wa::UserProcessInfo{pid, parentPid, name};
}

}  // namespace

TEST(UserProcessTree, AllModeListsProcessesWithoutSessions) {
    const std::vector<wa::UserProcessInfo> procs = {
        proc(10, "zeta.exe"), proc(20, "alpha.exe"), proc(30, "Beta.exe")};
    const std::vector<wa::LiveSessionView> sessions = {session(30, "Beta.exe", "Inactive")};
    const auto rows = wa::buildProcessTree(procs, sessions, false, 0);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].pid, 30u);
    EXPECT_EQ(rows[1].pid, 20u);
    EXPECT_EQ(rows[2].pid, 10u);
    EXPECT_TRUE(rows[1].sessions.empty());
}

TEST(UserProcessTree, AudioOnlyKeepsSessionOwners) {
    const std::vector<wa::UserProcessInfo> procs = {proc(10, "a.exe"), proc(20, "b.exe")};
    const std::vector<wa::LiveSessionView> sessions = {session(20, "b.exe", "Active")};
    const auto rows = wa::buildProcessTree(procs, sessions, true, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].pid, 20u);
}

TEST(UserProcessTree, GroupsSessionsByPidAndFlagsActive) {
    const std::vector<wa::UserProcessInfo> procs = {proc(7, "app.exe")};
    const std::vector<wa::LiveSessionView> sessions = {
        session(7, "app.exe", "Inactive"), session(8, "other.exe", "Inactive"),
        session(7, "app.exe", "Active")};
    const auto rows = wa::buildProcessTree(procs, sessions, false, 0);
    ASSERT_EQ(rows.size(), 2u);
    const auto it = std::find_if(rows.begin(), rows.end(), [](const auto& r) { return r.pid == 7u; });
    ASSERT_NE(it, rows.end());
    EXPECT_EQ(it->sessions, (std::vector<int>{0, 2}));
    EXPECT_TRUE(it->hasActiveSession);
    const auto other = std::find_if(rows.begin(), rows.end(), [](const auto& r) { return r.pid == 8u; });
    ASSERT_NE(other, rows.end());
    EXPECT_FALSE(other->hasActiveSession);
}

TEST(UserProcessTree, DropsHiddenAndZeroPid) {
    const std::vector<wa::UserProcessInfo> procs = {
        proc(0, "idle"), proc(5, "self.exe"), proc(6, "x.exe")};
    const std::vector<wa::LiveSessionView> sessions = {session(5, "self.exe", "Active")};
    const auto rows = wa::buildProcessTree(procs, sessions, false, 5);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].pid, 6u);
}

TEST(UserProcessEnumerator, IncludesCurrentProcess) {
    std::vector<wa::UserProcessInfo> procs;
    ASSERT_TRUE(wa::enumerateUserProcesses(procs));
    const uint32_t self = static_cast<uint32_t>(GetCurrentProcessId());
    const auto it = std::find_if(procs.begin(), procs.end(),
                                 [&](const auto& p) { return p.pid == self; });
    ASSERT_NE(it, procs.end());
    EXPECT_FALSE(it->name.empty());
    // Toolhelp always reports a parent PID for a normal user process.
    EXPECT_NE(it->parentPid, 0u);
}

TEST(UserProcessTree, GroupsSameNameUnderOneParent) {
    const std::vector<wa::UserProcessInfo> procs = {
        proc(1, "ChatGPT.exe", 99), proc(2, "chatgpt.exe", 99), proc(3, "solo.exe", 1),
        proc(4, "ChatGPT.exe", 1)};
    const std::vector<wa::LiveSessionView> sessions = {session(2, "chatgpt.exe", "Active")};
    const auto rows = wa::buildProcessTree(procs, sessions, false, 0);
    const auto groups = wa::groupProcessTreeByName(rows);
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[0].name, "chatgpt.exe");
    ASSERT_EQ(groups[0].processes.size(), 3u);
    EXPECT_TRUE(groups[0].hasAudio);
    EXPECT_TRUE(groups[0].hasActiveSession);
    EXPECT_EQ(groups[0].sessionCount, 1);
    // Root = Windows-tree root among same-name PIDs. pid 2 and 1 have parent 99 outside
    // the group; pid 2 wins (Active). pid 4 has parent 1 inside the group.
    EXPECT_EQ(groups[0].rootPid, 2u);
    EXPECT_EQ(groups[0].processes[0].pid, 2u);
    EXPECT_EQ(groups[1].name, "solo.exe");
    ASSERT_EQ(groups[1].processes.size(), 1u);
    EXPECT_EQ(groups[1].rootPid, 3u);
}

TEST(UserProcessTree, SingletonNameStaysOneProcessGroup) {
    const std::vector<wa::UserProcessInfo> procs = {proc(9, "only.exe", 1)};
    const auto groups = wa::groupProcessTreeByName(wa::buildProcessTree(procs, {}, false, 0));
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].processes.size(), 1u);
    EXPECT_EQ(groups[0].rootPid, 9u);
    EXPECT_FALSE(groups[0].hasAudio);
}

TEST(UserProcessTree, PickRootPrefersOutsideParent) {
    // 10 is parent of 11 and 12; all same name. Root should be 10.
    std::vector<wa::ProcessTreeRow> rows = {
        {11, 10, "app.exe", {}, false},
        {12, 10, "app.exe", {}, false},
        {10, 1, "app.exe", {}, false},
    };
    EXPECT_EQ(wa::pickNameGroupRootPid(rows), 10u);
}

TEST(UserProcessTree, SortByNamePidAndState) {
    auto groups = wa::groupProcessTreeByName(wa::buildProcessTree(
        {proc(30, "zeta.exe"), proc(10, "alpha.exe"), proc(20, "beta.exe", 1),
         proc(21, "beta.exe", 20)},
        {session(20, "beta.exe", "Active"), session(30, "zeta.exe", "Inactive")}, false, 0));
    ASSERT_EQ(groups.size(), 3u);

    wa::sortProcessNameGroups(groups, 0, true);
    EXPECT_EQ(groups[0].name, "alpha.exe");
    EXPECT_EQ(groups[1].name, "beta.exe");
    EXPECT_EQ(groups[2].name, "zeta.exe");

    wa::sortProcessNameGroups(groups, 1, true);
    EXPECT_EQ(groups[0].rootPid, 10u);  // alpha
    // beta root is 20 (parent of 21), zeta is 30
    EXPECT_EQ(groups[1].rootPid, 20u);
    EXPECT_EQ(groups[2].rootPid, 30u);

    wa::sortProcessNameGroups(groups, 5, true);  // Active first
    EXPECT_TRUE(groups[0].hasActiveSession);
    EXPECT_EQ(groups[0].name, "beta.exe");
}
