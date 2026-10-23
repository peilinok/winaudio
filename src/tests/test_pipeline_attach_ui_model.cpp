#include <gtest/gtest.h>
#include <string>
#include <unordered_map>
#include <vector>
#include "CallLogStore.h"
#include "PipelineAttachUiModel.h"
#include "PipelineGraph.h"

using namespace wa;

TEST(PipelineAttachUiModel, CopyLiveSessionPidToBuffer) {
    std::vector<LiveSessionView> rows(2);
    rows[0].processId = 100u;
    rows[1].processId = 4242u;
    char buf[32] = "stale";
    EXPECT_TRUE(pipeline_attach_ui::copyLiveSessionPidToBuffer(rows, 1, buf, sizeof(buf)));
    EXPECT_STREQ(buf, "4242");
    EXPECT_FALSE(pipeline_attach_ui::copyLiveSessionPidToBuffer(rows, -1, buf, sizeof(buf)));
    EXPECT_FALSE(pipeline_attach_ui::copyLiveSessionPidToBuffer(rows, 2, buf, sizeof(buf)));
    EXPECT_FALSE(pipeline_attach_ui::copyLiveSessionPidToBuffer(rows, 0, nullptr, 8));
}

TEST(PipelineAttachUiModel, ParseAttachPidRejectsZero) {
    uint32_t pid = 99;
    EXPECT_TRUE(pipeline_attach_ui::parseAttachPid("25060", pid));
    EXPECT_EQ(pid, 25060u);
    EXPECT_FALSE(pipeline_attach_ui::parseAttachPid("0", pid));
    EXPECT_FALSE(pipeline_attach_ui::parseAttachPid("abc", pid));
    EXPECT_FALSE(pipeline_attach_ui::parseAttachPid("", pid));
}

TEST(PipelineAttachUiModel, CanAttachRequiresNonzeroAndDetached) {
    EXPECT_TRUE(pipeline_attach_ui::canAttach(4242u, false));
    EXPECT_FALSE(pipeline_attach_ui::canAttach(0u, false));
    EXPECT_FALSE(pipeline_attach_ui::canAttach(4242u, true));
}

TEST(PipelineAttachUiModel, StatusForPidWithoutSessionRow) {
    EXPECT_EQ(pipeline_attach_ui::statusForPid(4242u, true, 4242u, 0u, false),
              PipelineAttachUiState::Attached);
    EXPECT_EQ(pipeline_attach_ui::statusForPid(4242u, false, 0u, 4242u, true),
              PipelineAttachUiState::Failed);
    EXPECT_EQ(pipeline_attach_ui::statusForPid(7u, true, 4242u, 4242u, true),
              PipelineAttachUiState::Detached);
    EXPECT_EQ(pipeline_attach_ui::statusForPid(0u, true, 4242u, 0u, false),
              PipelineAttachUiState::Detached);
}

TEST(PipelineAttachUiModel, HookStatusFollowsAttachPid) {
    LiveSessionView row{};
    row.processId = 4242u;
    row.deviceId = "dev";
    row.flow = PipelineFlow::Render;
    row.sessionInstanceId = "inst-a";
    std::unordered_map<std::string, PipelineAttachUiState> map;
    map[liveSessionAttachKey(row)] = PipelineAttachUiState::Detached;

    EXPECT_EQ(pipeline_attach_ui::hookStatusForRow(row, true, 4242u, 0u, false, map),
              PipelineAttachUiState::Attached);
    EXPECT_EQ(pipeline_attach_ui::hookStatusForRow(row, false, 0u, 4242u, true, map),
              PipelineAttachUiState::Failed);

    LiveSessionView other = row;
    other.processId = 7u;
    other.sessionInstanceId = "inst-b";
    EXPECT_EQ(pipeline_attach_ui::hookStatusForRow(other, true, 4242u, 0u, false, map),
              PipelineAttachUiState::Detached);
}

TEST(PipelineAttachUiModel, SessionHintOnlyWhenSelectedPidMatches) {
    std::vector<LiveSessionView> rows(1);
    rows[0].processId = 4242u;
    rows[0].deviceId = "dev-1";
    rows[0].flow = PipelineFlow::Render;
    rows[0].sessionInstanceId = "inst-1";
    std::string deviceId;
    PipelineFlow flow = PipelineFlow::Capture;
    std::string key;
    EXPECT_TRUE(pipeline_attach_ui::sessionHintForPid(rows, 0, 4242u, deviceId, flow, key));
    EXPECT_EQ(deviceId, "dev-1");
    EXPECT_EQ(flow, PipelineFlow::Render);
    EXPECT_EQ(key, "inst-1");
    EXPECT_FALSE(pipeline_attach_ui::sessionHintForPid(rows, 0, 7u, deviceId, flow, key));
    EXPECT_FALSE(pipeline_attach_ui::sessionHintForPid(rows, -1, 4242u, deviceId, flow, key));
}