#include <gtest/gtest.h>
#include "CallLogStore.h"
#include "PumpWindow.h"
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

TEST(CallLogStore, RecordingDoesNotGateObservation) {
    CallLogStore store;
    store.append({sampleCall()});
    EXPECT_EQ(store.size(), 1u);
    store.setRecording(true);
    store.append({sampleCall()});
    EXPECT_EQ(store.size(), 2u);
    store.setRecording(false);
    store.append({sampleCall()});
    EXPECT_EQ(store.size(), 3u);
}

TEST(CallLogStore, RecordingFreezesReceptionBoundaryAndCanIncludeHistoryAgain) {
    CallLogStore store;
    store.append({sampleCall(), sampleCall()});
    store.setRecording(true);
    store.append({sampleCall()});
    store.setRecording(false);
    store.append({sampleCall()});
    const auto frozen = store.freezeExport();
    ASSERT_EQ(frozen.records.size(), 3u);
    EXPECT_EQ(store.size(), 4u);
    store.setRecording(true);
    EXPECT_EQ(store.freezeExport().records.size(), 4u);
    store.append({sampleCall()});
    EXPECT_EQ(frozen.records.size(), 3u);
    EXPECT_EQ(frozen.records.front().get(), store.entries().front().get());
    EXPECT_EQ(frozen.records.front()->call.parameters[0].name, "ShareMode");
}

TEST(CallLogStore, ExplicitEmptyRecordingDoesNotFallBackToLaterHistory) {
    CallLogStore store;
    store.setRecording(true);
    store.setRecording(false);
    store.append({sampleCall()});
    EXPECT_EQ(store.size(), 1u);
    EXPECT_TRUE(store.freezeExport().records.empty());
}

TEST(CallLogStore, AttachSegmentsAndRoundsDoNotReuseRecordIdentity) {
    CallLogStore store;
    const auto a = store.registerAttach(7);
    EXPECT_EQ(store.registerAttach(7), a);
    const auto b = store.registerAttach(9);
    auto call = sampleCall();
    store.append(a, {call});
    store.append(b, {call});
    const auto kept = store.entries().front();
    EXPECT_EQ(kept->call.pid, 7u);
    EXPECT_EQ(store.entries().back()->call.pid, 9u);
    EXPECT_NE(kept->id, store.entries().back()->id);
    store.endAttach(a);
    const auto reconnected = store.registerAttach(7);
    EXPECT_NE(a, reconnected);
    EXPECT_TRUE(store.sessionRecords(7).empty());
    store.endAttach(reconnected);
    EXPECT_TRUE(store.sessionRecords(7).empty());
    store.append(b, {call});
    ASSERT_NE(store.find(kept->id), nullptr);
    EXPECT_EQ(store.find(kept->id), kept.get());
    EXPECT_EQ(store.size(), 3u);
    store.beginRound();
    EXPECT_EQ(store.find(kept->id), nullptr);
    EXPECT_TRUE(store.entries().empty());
    EXPECT_EQ(kept->call.parameters.front().name, "ShareMode");
    const auto fresh = store.registerAttach(7);
    store.append(fresh, {call});
    EXPECT_NE(store.entries().front()->id, kept->id);
}

TEST(CallLogStore, PumpWindowHasGlobalLimitAndExpiresOnDetachOrHide) {
    CallLogStore store;
    const auto first = store.registerAttach(7), second = store.registerAttach(9);
    std::vector<PumpCall> window;
    for (uint64_t i = 1; i <= 64; ++i) {
        auto call = sampleCall();
        call.method = "GetBuffer";
        call.pump = true;
        call.timeMs = static_cast<int64_t>(i);
        window.push_back({i, std::move(call)});
    }
    store.updatePump(first, window);
    store.updatePump(second, window);
    EXPECT_TRUE(store.pumpEntries().empty());
    store.setPumpVisible(true);
    ASSERT_EQ(store.pumpEntries().size(), 64u);
    const auto selected = store.pumpEntries().back()->id;
    EXPECT_EQ(selected.attach, second);
    EXPECT_NE(store.find(selected), nullptr);
    EXPECT_EQ(store.size(), 0u);
    EXPECT_TRUE(store.freezeExport().records.empty());
    store.endAttach(second);
    EXPECT_EQ(store.find(selected), nullptr);
    EXPECT_EQ(store.pumpEntries().size(), 64u);
    const auto remaining = store.pumpEntries().front()->id;
    store.setPumpVisible(false);
    EXPECT_EQ(store.find(remaining), nullptr);
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

TEST(CallLogStore, SessionSourcesMatchStableReferenceAndExcludePreviousAttach) {
    CallLogStore store;
    const auto local = store.registerAttach(7), foreign = store.registerAttach(9);
    auto a = sampleCall();
    a.timeMs = 20;
    a.raw = true;
    store.append(local, a);
    auto b = a;
    b.raw = false;
    b.hresult = 0;
    b.format = "44100/16/2";
    store.append(local, b); // Same timestamp: reception order wins.
    auto late = a;
    late.timeMs = 10;
    late.format = "96000/32f/2";
    store.append(local, late);
    auto other = a;
    other.format = "192000/32f/2";
    store.append(foreign, other);
    auto legacy = a;
    legacy.timeMs = 5;
    legacy.format = "legacy";
    store.append({legacy});
    const auto extract = extractHookedInitializeRefs(store.sessionRecords(7));
    auto reference = std::vector<HookedCall>{legacy, late, a, b};
    const auto expected = extractHookedInitializeDetailed(reference);
    EXPECT_EQ(extract.primaryStreamId, expected.primaryStreamId);
    EXPECT_EQ(extract.streamCount, expected.streamCount);
    EXPECT_EQ(extract.hint.format, expected.hint.format);
    EXPECT_EQ(extract.hint.raw, std::optional<bool>(false));
    EXPECT_EQ(extract.hint.hresult, std::optional<int32_t>(0));
    EXPECT_EQ(extractHookedInitializeRefs(store.sessionRecords(9)).hint.format, other.format);
    store.endAttach(local);
    EXPECT_EQ(extractHookedInitializeRefs(store.sessionRecords(7)).hint.format, b.format);
    store.registerAttach(7);
    EXPECT_EQ(extractHookedInitializeRefs(store.sessionRecords(7)).hint.format, legacy.format);
    EXPECT_EQ(store.size(), 5u);
}

TEST(CallLogStore, ExportIdentityAndFilenameComeFromFrozenSources) {
    CallLogStore store;
    const auto first = store.registerAttach(7);
    store.append(first, sampleCall());
    const auto single = store.freezeExport();
    EXPECT_EQ(CallLogStore::exportFileStem(single).find("calls-pid7-"), 0u);
    store.append(store.registerAttach(9), sampleCall());
    const auto multi = store.freezeExport();
    EXPECT_EQ(CallLogStore::exportFileStem(multi).find("calls-multi-"), 0u);
    const auto line = CallLogStore::toJsonLine(*single.records.front());
    EXPECT_NE(line.find("\"roundId\":1"), std::string::npos);
    EXPECT_NE(line.find("\"attachId\":1"), std::string::npos);
    EXPECT_NE(line.find("\"sequence\":1"), std::string::npos);
    EXPECT_NE(line.find("\"lane\":\"control\""), std::string::npos);
    EXPECT_NE(line.find("\"pid\":7"), std::string::npos);
    store.beginRound();
    EXPECT_EQ(single.records.size(), 1u);
    EXPECT_EQ(single.records.front()->call.parameters.front().name, "ShareMode");
    EXPECT_FALSE(store.recording());
    store.append({sampleCall()});
    EXPECT_EQ(store.exportCount(), 1u);
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

TEST(CallLogStore, QueryOrdersLateRecordsAndReusesControlWorkForPumpOnlyUpdates) {
    CallLogStore store;
    const auto attach = store.registerAttach(7);
    auto call = sampleCall();
    call.timeMs = 30;
    store.append(attach, call);
    const auto selected = store.entries().front()->id;
    call.timeMs = 10;
    store.append(attach, call);
    store.append(attach, call);
    auto view = store.view({});
    ASSERT_EQ(view.size(), 3u);
    EXPECT_EQ(view.at(0)->id.sequence, 2u);
    EXPECT_EQ(view.at(1)->id.sequence, 3u);
    EXPECT_EQ(view.position(selected), 2u);
    EXPECT_EQ(view.work().sorted, 3u);
    EXPECT_EQ(view.work().matched, 3u);
    view = store.view({});
    EXPECT_EQ(view.work().sorted, 0u);
    EXPECT_EQ(view.work().matched, 0u);
    call.method = "GetBuffer";
    call.pump = true;
    store.setPumpVisible(true);
    store.updatePump(attach, {{1, call}});
    view = store.view({});
    ASSERT_EQ(view.size(), 4u);
    EXPECT_EQ(view.at(2)->id.lane, CallLane::Pump); // Equal time: controls first.
    EXPECT_EQ(view.position(selected), 3u);
    EXPECT_EQ(view.work().sorted, 0u);
    EXPECT_EQ(view.work().matched, 0u);
    const auto pump = view.at(2)->id;
    store.setPumpVisible(false);
    view = store.view({});
    EXPECT_EQ(view.work().matched, 0u);
    EXPECT_EQ(view.size(), 3u);
    EXPECT_EQ(view.position(pump), CallLogStore::kNotFound);
    EXPECT_EQ(store.find(pump), nullptr);
    EXPECT_EQ(store.find(selected)->id, selected);
}

TEST(CallLogStore, QueryUsesExistingSameFieldTokensAndKeepsHiddenControlDetails) {
    CallLogStore store;
    auto call = sampleCall();
    call.hresult = 1;
    call.args = "alpha\nbeta,\"quoted\"";
    store.append({call});
    const auto selected = store.entries().front()->id;
    auto view = store.view({"init alpha", "", false});
    EXPECT_EQ(view.size(), 0u); // Tokens cannot be assembled across Call and Args.
    EXPECT_NE(store.find(selected), nullptr);
    view = store.view({"alpha beta", "iaudioclient", true});
    ASSERT_EQ(view.size(), 1u); // S_FALSE remains a nonzero HRESULT.
    EXPECT_EQ(view.at(0), store.find(selected));
    EXPECT_EQ(view.position(selected), 0u);
    EXPECT_EQ(view.work().sorted, 0u);
    EXPECT_EQ(view.work().matched, 1u);
    EXPECT_EQ(store.find(selected)->call.args, call.args);
    EXPECT_EQ(call_log_filter::singleLine(call.args), "alpha beta,\"quoted\"");
    EXPECT_NE(CallLogStore::toJsonLine(*store.find(selected)).find("alpha\\nbeta"), std::string::npos);
    EXPECT_EQ(store.ifaces(), std::vector<std::string>{"IAudioClient"});
    view = store.view({"alpha beta", "iaudioclient", true});
    EXPECT_EQ(view.work().matched, 0u);
}

TEST(CallLogStore, PumpIfaceOptionsExpireWithoutChangingControlQuery) {
    CallLogStore store;
    const auto attach = store.registerAttach(7);
    store.append(attach, sampleCall());
    store.view({});
    auto pump = sampleCall();
    pump.iface = "IAudioRenderClient";
    pump.method = "GetBuffer";
    store.setPumpVisible(true);
    store.updatePump(attach, {{1, pump}});
    EXPECT_EQ(store.ifaces().size(), 2u);
    EXPECT_EQ(store.view({}).work().matched, 0u);
    store.endAttach(attach);
    EXPECT_EQ(store.ifaces(), std::vector<std::string>{"IAudioClient"});
    EXPECT_EQ(store.view({}).work().matched, 0u);
}
