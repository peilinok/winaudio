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

}  // namespace

TEST(UserProcessTree, AllModeListsProcessesWithoutSessions) {
    const std::vector<wa::UserProcessInfo> procs = {{10, "zeta.exe"}, {20, "alpha.exe"}, {30, "Beta.exe"}};
    const std::vector<wa::LiveSessionView> sessions = {session(30, "Beta.exe", "Inactive")};
    const auto rows = wa::buildProcessTree(procs, sessions, false, 0);
    ASSERT_EQ(rows.size(), 3u);
    // Audio process first, then case-insensitive name.
    EXPECT_EQ(rows[0].pid, 30u);
    EXPECT_EQ(rows[1].pid, 20u);
    EXPECT_EQ(rows[2].pid, 10u);
    EXPECT_TRUE(rows[1].sessions.empty());
}

TEST(UserProcessTree, AudioOnlyKeepsSessionOwners) {
    const std::vector<wa::UserProcessInfo> procs = {{10, "a.exe"}, {20, "b.exe"}};
    const std::vector<wa::LiveSessionView> sessions = {session(20, "b.exe", "Active")};
    const auto rows = wa::buildProcessTree(procs, sessions, true, 0);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].pid, 20u);
}

TEST(UserProcessTree, GroupsSessionsByPidAndFlagsActive) {
    const std::vector<wa::UserProcessInfo> procs = {{7, "app.exe"}};
    const std::vector<wa::LiveSessionView> sessions = {
        session(7, "app.exe", "Inactive"), session(8, "other.exe", "Inactive"), session(7, "app.exe", "Active")};
    const auto rows = wa::buildProcessTree(procs, sessions, false, 0);
    ASSERT_EQ(rows.size(), 2u);
    const auto it = std::find_if(rows.begin(), rows.end(), [](const auto& r) { return r.pid == 7u; });
    ASSERT_NE(it, rows.end());
    EXPECT_EQ(it->sessions, (std::vector<int>{0, 2}));
    EXPECT_TRUE(it->hasActiveSession);
    // A session PID missing from the process snapshot is still listed.
    const auto other = std::find_if(rows.begin(), rows.end(), [](const auto& r) { return r.pid == 8u; });
    ASSERT_NE(other, rows.end());
    EXPECT_FALSE(other->hasActiveSession);
}

TEST(UserProcessTree, DropsHiddenAndZeroPid) {
    const std::vector<wa::UserProcessInfo> procs = {{0, "idle"}, {5, "self.exe"}, {6, "x.exe"}};
    const std::vector<wa::LiveSessionView> sessions = {session(5, "self.exe", "Active")};
    const auto rows = wa::buildProcessTree(procs, sessions, false, 5);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].pid, 6u);
}

TEST(UserProcessEnumerator, IncludesCurrentProcess) {
    std::vector<wa::UserProcessInfo> procs;
    ASSERT_TRUE(wa::enumerateUserProcesses(procs));
    const uint32_t self = static_cast<uint32_t>(GetCurrentProcessId());
    const auto it = std::find_if(procs.begin(), procs.end(), [&](const auto& p) { return p.pid == self; });
    ASSERT_NE(it, procs.end());
    EXPECT_FALSE(it->name.empty());
}

TEST(UserProcessTree, GroupsSameNameUnderOneParent) {
    const std::vector<wa::UserProcessInfo> procs = {
        {1, "ChatGPT.exe"}, {2, "chatgpt.exe"}, {3, "solo.exe"}, {4, "ChatGPT.exe"}};
    const std::vector<wa::LiveSessionView> sessions = {session(2, "chatgpt.exe", "Active")};
    const auto rows = wa::buildProcessTree(procs, sessions, false, 0);
    const auto groups = wa::groupProcessTreeByName(rows);
    ASSERT_EQ(groups.size(), 2u);
    // Audio-owning name first.
    EXPECT_EQ(groups[0].name, "chatgpt.exe");
    ASSERT_EQ(groups[0].processes.size(), 3u);
    EXPECT_TRUE(groups[0].hasAudio);
    EXPECT_TRUE(groups[0].hasActiveSession);
    EXPECT_EQ(groups[0].sessionCount, 1);
    // Within the name group: audio PID first, then ascending pid.
    EXPECT_EQ(groups[0].processes[0].pid, 2u);
    EXPECT_EQ(groups[0].processes[1].pid, 1u);
    EXPECT_EQ(groups[0].processes[2].pid, 4u);
    EXPECT_EQ(groups[1].name, "solo.exe");
    ASSERT_EQ(groups[1].processes.size(), 1u);
}

TEST(UserProcessTree, SingletonNameStaysOneProcessGroup) {
    const std::vector<wa::UserProcessInfo> procs = {{9, "only.exe"}};
    const auto groups = wa::groupProcessTreeByName(wa::buildProcessTree(procs, {}, false, 0));
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].processes.size(), 1u);
    EXPECT_FALSE(groups[0].hasAudio);
}
