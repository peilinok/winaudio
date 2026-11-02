#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <set>
#include <chrono>
#include <memory>
namespace { void transportSchedule(const char* event); }
#define WA_HOOK_TRANSPORT_OBSERVE(stage) transportSchedule(#stage)
#include "PumpWindow.h"
using wa::hook_ipc::Ring;
using wa::hook_ipc::CallPod;

namespace {
thread_local bool pauseReservation = false;
thread_local const char* pauseAt = "reserved";
std::atomic<bool> reservationHeld{false}, releaseReservation{false};
wa::hook_ipc::LaneCounters* contendCounter = nullptr;
std::atomic<unsigned> contentionAttempts{0};
void transportSchedule(const char* event) {
    if (contendCounter && !std::strcmp(event, "counter_attempt")) {
        wa::hook_ipc::increment64(contendCounter->rejected);
        ++contentionAttempts;
    }
    if (pauseReservation && !std::strcmp(event, pauseAt)) {
        reservationHeld = true;
        while (!releaseReservation.load()) std::this_thread::yield();
    }
}

struct MappingViews {
    HANDLE map = nullptr;
    Ring* writer = nullptr;
    Ring* reader = nullptr;
    explicit MappingViews(uint32_t pid = 0) {
        map = pid ? wa::hook_ipc::createHookMapping(pid) : CreateFileMappingW(
            INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Ring), nullptr);
        if (!map) return;
        writer = static_cast<Ring*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring)));
        reader = static_cast<Ring*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring)));
        if (writer) {
            wa::hook_ipc::initializeRing(*writer);
            writer->captureEnabled = writer->pumpEnabled = 1;
            wa::hook_ipc::publishMonitorState(*writer, wa::kAllMonitorMethods, 1);
        }
    }
    ~MappingViews() {
        if (writer) UnmapViewOfFile(writer);
        if (reader) UnmapViewOfFile(reader);
        if (map) CloseHandle(map);
    }
};

CallPod patterned(unsigned id, bool pump, uint32_t generation = 1) {
    CallPod pod{};
    pod.streamId = id;
    pod.pump = pump;
    pod.observationGeneration = generation;
    strcpy_s(pod.iface, pump ? "IAudioRenderClient" : "IAudioClient");
    strcpy_s(pod.method, pump ? "ReleaseBuffer" : "Initialize");
    sprintf_s(pod.args, "source=%u", id);
    pod.hresult = id % 3 == 0 ? E_FAIL : id % 3 == 1 ? S_FALSE : S_OK;
    pod.xrun = (id % 7 == 0);
    pod.hasCategory = pod.hasRaw = pod.hasMatchFormat = pod.hasExclusive = pod.hasFormat = 1;
    pod.raw = pod.matchFormat = pod.exclusive = static_cast<uint8_t>(id % 2);
    strcpy_s(pod.category, "Communications");
    strcpy_s(pod.format, "48000/32f/2");
    for (unsigned i = 0; i < sizeof(pod.argBlob); ++i) pod.argBlob[i] = static_cast<uint8_t>(id + i);
    pod.argBlobLen = sizeof(pod.argBlob);
    return pod;
}
void verifyPattern(const CallPod& pod, std::set<unsigned>& unique) {
    EXPECT_TRUE(unique.insert(pod.streamId).second) << "Duplicate source record " << pod.streamId;
    const auto expected = patterned(pod.streamId, pod.pump != 0, pod.observationGeneration);
    EXPECT_EQ(pod.hresult, expected.hresult);
    EXPECT_EQ(pod.xrun, expected.xrun);
    EXPECT_STREQ(pod.iface, expected.iface);
    EXPECT_STREQ(pod.method, expected.method);
    EXPECT_STREQ(pod.args, expected.args);
    EXPECT_EQ(pod.hasCategory, 1u);
    EXPECT_EQ(pod.hasRaw, 1u);
    EXPECT_EQ(pod.hasMatchFormat, 1u);
    EXPECT_EQ(pod.hasExclusive, 1u);
    EXPECT_EQ(pod.hasFormat, 1u);
    EXPECT_EQ(pod.raw, expected.raw);
    EXPECT_EQ(pod.matchFormat, expected.matchFormat);
    EXPECT_EQ(pod.exclusive, expected.exclusive);
    EXPECT_STREQ(pod.category, expected.category);
    EXPECT_STREQ(pod.format, expected.format);
    EXPECT_EQ(pod.argBlobLen, sizeof(pod.argBlob));
    EXPECT_EQ(std::memcmp(pod.argBlob, expected.argBlob, sizeof(pod.argBlob)), 0);
}
bool awaitHeld() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!reservationHeld.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    return reservationHeld.load();
}
}
#define WA_HOOK_OBSERVE(event) transportSchedule(#event)
#define DllMain TransportUnusedDllMain
#include "../hook/WinAudioHook.cpp"
#undef DllMain
#undef WA_HOOK_OBSERVE

TEST(HookTransport, SlowReservationCannotMasqueradeAsLatestSlotGeneration) {
    auto mapping = std::make_unique<Ring>();
    auto& ring = *mapping;
    g_ring = &ring;
    ring.captureEnabled = ring.pumpEnabled = 1;
    wa::hook_ipc::publishMonitorState(ring, wa::kAllMonitorMethods, 0);
    wa::PumpWindow window;
    window.begin(ring);
    reservationHeld = releaseReservation = false;
    std::thread slow([] {
        pauseReservation = true;
        emitSimple(777, "IAudioRenderClient", "ReleaseBuffer", "slow", S_OK, 0);
    });
    EXPECT_TRUE(awaitHeld());
    std::vector<std::thread> writers;
    for (unsigned writer = 0; writer < 4; ++writer) writers.emplace_back([writer] {
        for (unsigned row = 0; row < wa::hook_ipc::kPumpCap; ++row)
            emitSimple(100 + writer, "IAudioRenderClient", "ReleaseBuffer", "complete", S_OK, 0);
    });
    for (auto& writer : writers) writer.join();
    const auto before = window.poll(ring);
    releaseReservation = true;
    slow.join();
    const auto after = window.poll(ring);
    ASSERT_FALSE(before.empty());
    for (const auto& call : after) {
        if (call.call.streamId == 777)
            EXPECT_LT(call.sequence, before.front().sequence)
                << "A delayed reservation was accepted as a newer complete publication";
    }
}

TEST(HookTransport, HeldWriterSurvivesFourWritersAcrossCapacityWithoutTearing) {
    MappingViews views;
    ASSERT_TRUE(views.writer && views.reader);
    g_ring = views.writer;
    wa::HookTransportReader receiver;
    receiver.begin(*views.reader, true, 1);
    reservationHeld = releaseReservation = false;
    std::thread slow([] {
        pauseAt = "acquired";
        pauseReservation = true;
        emit(patterned(900000, true), 1);
    });
    EXPECT_TRUE(awaitHeld());
    EXPECT_EQ(receiver.snapshot(*views.reader).pending, 1u);
    std::atomic<unsigned> remaining{4};
    std::vector<std::thread> writers;
    for (unsigned writer = 0; writer < 4; ++writer) writers.emplace_back([&, writer] {
        for (unsigned row = 1; row <= 512; ++row) emit(patterned(writer * 10000 + row, true), 1);
        --remaining;
    });
    std::set<unsigned> unique;
    const auto receive = [&](const CallPod& pod, uint64_t) { verifyPattern(pod, unique); };
    while (remaining.load()) receiver.poll(*views.reader, receive);
    for (auto& writer : writers) writer.join();
    receiver.poll(*views.reader, receive);
    const auto before = receiver.snapshot(*views.reader);
    EXPECT_GT(before.writeRejected, 0u);
    EXPECT_EQ(before.pending, 1u);
    releaseReservation = true;
    slow.join();
    receiver.poll(*views.reader, receive);
    const auto after = receiver.snapshot(*views.reader);
    EXPECT_EQ(after.pending, 0u);
    EXPECT_EQ(unique.count(900000), 1u);
    EXPECT_EQ(unique.size() + after.overwritten + after.writeRejected, 2049u);
    EXPECT_FALSE(after.unknown);
    // High-frequency traffic never consumes control capacity or loss counts.
    wa::HookTransportReader control;
    control.begin(*views.reader, false, 1);
    emit(patterned(700000, false), 1);
    unsigned controls = 0;
    control.poll(*views.reader, [&](const CallPod& pod, uint64_t) { EXPECT_EQ(pod.streamId, 700000u); ++controls; });
    EXPECT_EQ(controls, 1u);
    EXPECT_EQ(control.snapshot(*views.reader).overwritten, 0u);
}

TEST(HookTransport, ReaderOwnershipRejectsWraparoundWriterWithoutChangingPayload) {
    MappingViews views;
    ASSERT_TRUE(views.writer && views.reader);
    ASSERT_TRUE(wa::hook_ipc::writeCall(*views.writer, patterned(1, true)));
    reservationHeld = releaseReservation = false;
    CallPod read{};
    uint64_t sequence = 99;
    std::thread reader([&] {
        pauseAt = "reading";
        pauseReservation = true;
        EXPECT_TRUE(wa::hook_ipc::readCall(*views.reader, true, 0, 1, read, sequence));
    });
    EXPECT_TRUE(awaitHeld());
    for (unsigned id = 2; id <= 65; ++id) wa::hook_ipc::writeCall(*views.writer, patterned(id, true));
    releaseReservation = true;
    reader.join();
    EXPECT_EQ(read.streamId, 1u);
    EXPECT_EQ(sequence, 0u);
    EXPECT_EQ(static_cast<uint32_t>(wa::hook_ipc::atomicRead64(views.reader->pumpCounters.rejected)), 1u);
}

TEST(HookTransport, OldReservationCannotAffectNewSegmentLossOrPayload) {
    MappingViews views;
    ASSERT_TRUE(views.writer && views.reader);
    g_ring = views.writer;
    reservationHeld = releaseReservation = false;
    std::thread old([] { pauseReservation = true; emit(patterned(900000, true), 1); });
    EXPECT_TRUE(awaitHeld());
    wa::hook_ipc::publishMonitorState(*views.writer, wa::kAllMonitorMethods, 2);
    wa::HookTransportReader receiver;
    receiver.begin(*views.reader, true, 2);
    for (unsigned id = 1; id <= 128; ++id) emit(patterned(id, true, 2), 2);
    const auto before = receiver.snapshot(*views.reader);
    releaseReservation = true;
    old.join();
    std::set<unsigned> unique;
    receiver.poll(*views.reader, [&](const CallPod& pod, uint64_t) {
        EXPECT_EQ(pod.observationGeneration, 2u);
        EXPECT_NE(pod.streamId, 900000u);
        verifyPattern(pod, unique);
    });
    const auto after = receiver.snapshot(*views.reader);
    EXPECT_EQ(before.writeRejected, after.writeRejected);
    EXPECT_EQ(before.overwritten, after.overwritten);
    EXPECT_EQ(unique.size() + after.overwritten + after.writeRejected, 128u);
}

TEST(HookTransport, TicketWrapAndLossCounterWrapKeepIdentityAndExposeUnknown) {
    MappingViews views;
    ASSERT_TRUE(views.writer && views.reader);
    wa::hook_ipc::atomicStore64(views.writer->pumpCounters.reserved, UINT64_MAX - 1);
    wa::HookTransportReader receiver;
    receiver.begin(*views.reader, true, 1);
    for (unsigned id = 1; id <= 4; ++id) ASSERT_TRUE(wa::hook_ipc::writeCall(*views.writer, patterned(id, true)));
    std::set<unsigned> unique;
    receiver.poll(*views.reader, [&](const CallPod& pod, uint64_t) { verifyPattern(pod, unique); });
    EXPECT_EQ(unique.size(), 4u);
    receiver.poll(*views.reader, [&](const CallPod&, uint64_t) { ADD_FAILURE() << "Duplicate after ticket wrap"; });
    auto& lane = views.writer->pumpCounters;
    wa::hook_ipc::atomicStore64(lane.rejected, wa::hook_ipc::slotOwner(1, UINT32_MAX));
    wa::hook_ipc::countLoss(*views.writer, lane.rejected, lane, 1);
    EXPECT_TRUE(receiver.snapshot(*views.reader).unknown);
    ++views.writer->transportEpoch;
    EXPECT_TRUE(receiver.snapshot(*views.reader).unknown);
    // Generation comparison uses the atomically published authority, including
    // wrap; a late UINT32_MAX event must never switch a generation-zero counter.
    wa::hook_ipc::publishMonitorState(*views.writer, wa::kAllMonitorMethods, 0);
    wa::hook_ipc::countLoss(*views.writer, lane.rejected, lane, 0);
    wa::hook_ipc::countLoss(*views.writer, lane.rejected, lane, UINT32_MAX);
    EXPECT_EQ(wa::hook_ipc::atomicRead64(lane.rejected), 1u);
}

TEST(HookTransport, BoundedCounterContentionReportsUnknownInsteadOfExactZero) {
    MappingViews views;
    ASSERT_TRUE(views.writer && views.reader);
    wa::HookTransportReader receiver;
    receiver.begin(*views.reader, true, 1);
    contendCounter = &views.writer->pumpCounters;
    contentionAttempts = 0;
    wa::hook_ipc::countLoss(*views.writer, contendCounter->rejected, *contendCounter, 1);
    contendCounter = nullptr;
    EXPECT_EQ(contentionAttempts.load(), 8u);
    EXPECT_TRUE(receiver.snapshot(*views.reader).unknown);
}
