#include <gtest/gtest.h>
#include "CallLogStore.h"
#include <cstdio>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

using namespace wa;

static HookedCall sampleCall() {
    HookedCall c;
    c.timeMs = 123;
    c.iface = "IAudioClient";
    c.method = "Initialize";
    c.args = "ShareMode=SHARED StreamFlags=0x0 hnsBufferDuration=100000 hnsPeriodicity=0 fmt=48000/32f/2";
    c.hresult = 0;
    c.streamId = 42;
    c.format = "48000/32f/2";
    c.exclusive = false;
    CallArg share;
    share.type = "AUDCLNT_SHAREMODE";
    share.name = "ShareMode";
    share.pre = "AUDCLNT_SHAREMODE_SHARED";
    share.post = share.pre;
    c.parameters.push_back(share);
    CallArg ret;
    ret.dir = 3;
    ret.type = "HRESULT";
    ret.name = "Return";
    ret.pre = "S_OK";
    ret.post = "S_OK";
    c.parameters.push_back(ret);
    return c;
}

TEST(CallLogStore, FormatHresultSymbolic) {
    EXPECT_EQ(CallLogStore::formatHresult(0), "S_OK");
    EXPECT_EQ(CallLogStore::formatHresultHex(0), "0x00000000");
    EXPECT_EQ(CallLogStore::formatHresult(static_cast<int32_t>(0x80004005)), "E_FAIL");
}

TEST(CallLogStore, JsonLineIncludesArgsAndHr) {
    const std::string line = CallLogStore::toJsonLine(sampleCall());
    EXPECT_NE(line.find("\"method\":\"Initialize\""), std::string::npos);
    EXPECT_NE(line.find("hnsBufferDuration=100000"), std::string::npos);
    EXPECT_NE(line.find("\"hr\":\"S_OK\""), std::string::npos);
    EXPECT_NE(line.find("\"parameters\":["), std::string::npos);
    EXPECT_NE(line.find("ShareMode"), std::string::npos);
    EXPECT_NE(line.find("AUDCLNT_SHAREMODE_SHARED"), std::string::npos);
}

TEST(CallLogStore, RecordingGatesAppend) {
    CallLogStore store;
    store.append({sampleCall()});
    EXPECT_EQ(store.size(), 0u);
    store.setRecording(true);
    store.append({sampleCall()});
    EXPECT_EQ(store.size(), 1u);
    store.setRecording(false);
    store.append({sampleCall()});
    EXPECT_EQ(store.size(), 1u);
}

TEST(CallLogStore, SaveJsonlRoundTrip) {
    CallLogStore store;
    store.setRecording(true);
    store.append({sampleCall()});
    wchar_t tmp[MAX_PATH] = {};
    ASSERT_TRUE(GetTempPathW(MAX_PATH, tmp));
    std::wstring path = std::wstring(tmp) + L"winaudio-calllog-test.jsonl";
    ASSERT_TRUE(store.saveJsonl(path));
    FILE* f = nullptr;
    ASSERT_EQ(_wfopen_s(&f, path.c_str(), L"rb"), 0);
    ASSERT_TRUE(f);
    char buf[512] = {};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    DeleteFileW(path.c_str());
    ASSERT_GT(n, 0u);
    const std::string body(buf, n);
    EXPECT_NE(body.find("Initialize"), std::string::npos);
    EXPECT_NE(body.find("S_OK"), std::string::npos);
    EXPECT_NE(body.find("parameters"), std::string::npos);
}

TEST(CallLogStore, LiveSessionAttachKeyPrefersInstanceId) {
    LiveSessionView row;
    row.processId = 7;
    row.deviceId = "dev";
    row.flow = PipelineFlow::Capture;
    row.sessionInstanceId = "inst-1";
    EXPECT_EQ(liveSessionAttachKey(row), "inst-1");
    row.sessionInstanceId.clear();
    EXPECT_EQ(liveSessionAttachKey(row), "7|dev|C");
}

TEST(CallLogStore, AttachUiStateNames) {
    EXPECT_STREQ(pipelineAttachUiStateName(PipelineAttachUiState::Detached), "Detached");
    EXPECT_STREQ(pipelineAttachUiStateName(PipelineAttachUiState::Attached), "Attached");
    EXPECT_STREQ(pipelineAttachUiStateName(PipelineAttachUiState::Failed), "Failed");
}