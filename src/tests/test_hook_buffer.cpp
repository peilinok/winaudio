#include <gtest/gtest.h>
#include <cstring>

namespace {
void observeHook(const char* event);
}
#define WA_HOOK_OBSERVE(event) observeHook(#event)
#include "../probe/HookBufferHarness.h"
#undef WA_HOOK_OBSERVE

namespace {
unsigned prepared = 0, formatted = 0, timestamps = 0, writes = 0;
const char* closeAt = nullptr;
int closeGate = 0;
void* serviceOutput = nullptr;
HRESULT STDMETHODCALLTYPE fakeGetService(IAudioClient*, REFIID, void** out) {
    *out = serviceOutput;
    return S_OK;
}
void observeHook(const char* event) {
    if (!std::strcmp(event, "prepare")) ++prepared;
    if (!std::strcmp(event, "format")) ++formatted;
    if (!std::strcmp(event, "timestamp")) ++timestamps;
    if (!std::strcmp(event, "write")) ++writes;
    if (closeAt && !std::strcmp(event, closeAt)) {
        if (closeGate == 0)
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&hook_harness::ring.captureEnabled), 0);
        else if (closeGate == 1)
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&hook_harness::ring.pumpEnabled), 0);
        else
            InterlockedExchange(&g_quiet, 1);
    }
}
void reset(int state = 5) {
    hook_harness::reset(state);
    prepared = formatted = timestamps = writes = 0;
    closeAt = nullptr;
    closeGate = 0;
}
void enableAfterOriginal() {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&hook_harness::ring.pumpEnabled), 1);
}
void disableAfterOriginal() {
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&hook_harness::ring.pumpEnabled), 0);
}
}

TEST(HookBuffer, ClosedGatesCallOriginalWithoutRecordWork) {
    for (int entry = 0; entry < 4; ++entry) {
        for (int state = 1; state < 5; ++state) {
            SCOPED_TRACE(::testing::Message() << entry << ":" << state);
            reset(state);
            hook_harness::result = E_FAIL;
            EXPECT_EQ(hook_harness::invoke(entry), E_FAIL);
            EXPECT_EQ(hook_harness::calls, 1u);
            EXPECT_EQ(prepared, 0u);
            EXPECT_EQ(formatted, 0u);
            EXPECT_EQ(timestamps, 0u);
            EXPECT_EQ(writes, 0u);
            EXPECT_EQ(hook_harness::ring.pumpXruns, 0u);
            EXPECT_EQ(hook_harness::ring.pumpWriteIndex, 0u);
        }
    }
}

TEST(HookBuffer, EnabledPreservesArgumentsOutputsReturnAndXrun) {
    for (int entry = 0; entry < 4; ++entry) {
        for (HRESULT hr : {S_OK, S_FALSE, E_FAIL}) {
            SCOPED_TRACE(::testing::Message() << entry << ":" << hr);
            reset();
            hook_harness::result = hr;
            EXPECT_EQ(hook_harness::invoke(entry), hr);
            EXPECT_EQ(hook_harness::calls, 1u);
            EXPECT_EQ(hook_harness::seenSelf, reinterpret_cast<void*>(static_cast<uintptr_t>(entry < 2 ? 0x1234 : 0x5678)));
            if (entry == 0) {
                EXPECT_EQ(hook_harness::data, hook_harness::bytes);
                EXPECT_EQ(hook_harness::frames, 137u);
                EXPECT_EQ(hook_harness::flags, 0u);
                EXPECT_EQ(hook_harness::position, 123456u);
                EXPECT_EQ(hook_harness::qpc, 654321u);
            } else {
                EXPECT_EQ(hook_harness::seenFrames, static_cast<UINT32>(137 + entry * 2));
                if (entry == 2) EXPECT_EQ(hook_harness::data, hook_harness::bytes);
                if (entry == 3) EXPECT_EQ(hook_harness::seenFlags, AUDCLNT_BUFFERFLAGS_SILENT);
            }
            EXPECT_EQ(prepared, 1u);
            EXPECT_EQ(formatted, 1u);
            EXPECT_EQ(timestamps, 1u);
            EXPECT_EQ(writes, 1u);
            EXPECT_EQ(hook_harness::ring.pumpWriteIndex, 1u);
            EXPECT_EQ(hook_harness::ring.pumpXruns, FAILED(hr) ? 1u : 0u);
            const auto& pod = hook_harness::ring.pumpSlots[0];
            EXPECT_EQ(pod.hresult, hr);
            EXPECT_STREQ(pod.method, entry % 2 ? "ReleaseBuffer" : "GetBuffer");
            EXPECT_STREQ(pod.iface, entry < 2 ? "IAudioCaptureClient" : "IAudioRenderClient");
        }
    }
}

TEST(HookBuffer, CaptureDiscontinuityCountsOnceAndSwitchesKeepWindow) {
    reset();
    hook_harness::outputFlags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
    hook_harness::result = E_FAIL;
    hook_harness::invoke(0);
    EXPECT_EQ(hook_harness::ring.pumpXruns, 1u);
    hook_harness::result = S_OK;
    hook_harness::invoke(0);
    EXPECT_EQ(hook_harness::ring.pumpXruns, 2u);
    disableAfterOriginal();
    hook_harness::invoke(0);
    EXPECT_EQ(hook_harness::ring.pumpXruns, 2u);
    EXPECT_EQ(hook_harness::ring.pumpWriteIndex, 2u);
}

TEST(HookBuffer, SamplesAfterOriginalAndRechecksBeforeTimestamp) {
    for (int entry = 0; entry < 4; ++entry) {
        reset(4);
        hook_harness::afterOriginal = enableAfterOriginal;
        hook_harness::invoke(entry);
        EXPECT_EQ(writes, 1u);
        reset();
        hook_harness::afterOriginal = disableAfterOriginal;
        hook_harness::invoke(entry);
        EXPECT_EQ(prepared, 0u);
        for (int gate = 0; gate < 3; ++gate) {
            reset();
            closeAt = "precommit";
            closeGate = gate;
            hook_harness::result = E_FAIL;
            hook_harness::invoke(entry);
            EXPECT_EQ(prepared, 1u);
            EXPECT_EQ(timestamps, 0u);
            EXPECT_EQ(writes, 0u);
            EXPECT_EQ(hook_harness::ring.pumpXruns, 0u);
            reset();
            closeAt = "accepted";
            closeGate = gate;
            hook_harness::invoke(entry);
            EXPECT_EQ(writes, 1u); // In-flight work beyond the final check may complete.
        }
    }
}

TEST(HookBuffer, MissingOriginalAndNullOutputsRemainSafe) {
    reset();
    g_capGet = nullptr;
    g_capRel = nullptr;
    g_renGet = nullptr;
    g_renRel = nullptr;
    for (int entry = 0; entry < 4; ++entry) EXPECT_EQ(hook_harness::invoke(entry), E_UNEXPECTED);
    EXPECT_EQ(hook_harness::calls, 0u);
    reset();
    EXPECT_EQ(HookCapGetBuffer(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr), S_OK);
    EXPECT_EQ(HookRenGetBuffer(nullptr, 0, nullptr), S_OK);
}

TEST(HookBuffer, ControlDoesNotRequirePumpAndSharedFlagsKeepLayout) {
    reset(4);
    emitSimple(7, "IAudioClient", "Start", "", S_OK);
    EXPECT_EQ(hook_harness::ring.writeIndex, 1u);
    EXPECT_EQ(hook_harness::ring.pumpWriteIndex, 0u);
    EXPECT_EQ(wa::hook_ipc::kRingLayout, 5u);
    EXPECT_EQ(sizeof(hook_harness::ring.captureEnabled), 4u);
    EXPECT_EQ(sizeof(hook_harness::ring.pumpEnabled), 4u);
    EXPECT_EQ(offsetof(Ring, captureEnabled), 36u);
    EXPECT_EQ(offsetof(Ring, pumpEnabled), 16u);
    EXPECT_EQ(offsetof(Ring, captureEnabled) % alignof(LONG), 0u);
    EXPECT_EQ(sizeof(CallPod), 1040u);
    EXPECT_EQ(sizeof(Ring), 2197032u);
}

TEST(HookBuffer, GatesAndRecordsAreVisibleAcrossTwoLocalMappingViews) {
    reset();
    struct Mapping {
        HANDLE handle = nullptr;
        Ring* controller = nullptr;
        Ring* hook = nullptr;
        ~Mapping() {
            g_ring = nullptr;
            if (hook) UnmapViewOfFile(hook);
            if (controller) UnmapViewOfFile(controller);
            if (handle) CloseHandle(handle);
        }
    } mapping;
    mapping.handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                        0, sizeof(Ring), nullptr);
    ASSERT_NE(mapping.handle, nullptr);
    mapping.controller = static_cast<Ring*>(MapViewOfFile(
        mapping.handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring)));
    ASSERT_NE(mapping.controller, nullptr);
    mapping.hook = static_cast<Ring*>(MapViewOfFile(
        mapping.handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring)));
    ASSERT_NE(mapping.hook, nullptr);
    ASSERT_NE(mapping.controller, mapping.hook);
    g_ring = mapping.hook;
    for (int entry = 0; entry < 4; ++entry) {
        SCOPED_TRACE(entry);
        const auto index = mapping.controller->pumpWriteIndex;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&mapping.controller->captureEnabled), 1);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&mapping.controller->pumpEnabled), 1);
        EXPECT_EQ(hook_harness::invoke(entry), S_OK);
        EXPECT_EQ(mapping.controller->pumpWriteIndex, index + 1);
        EXPECT_STREQ(mapping.controller->pumpSlots[index].method,
                     entry % 2 ? "ReleaseBuffer" : "GetBuffer");
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&mapping.controller->pumpEnabled), 0);
        EXPECT_EQ(hook_harness::invoke(entry), S_OK);
        EXPECT_EQ(mapping.controller->pumpWriteIndex, index + 1);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&mapping.controller->pumpEnabled), 1);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&mapping.controller->captureEnabled), 0);
        EXPECT_EQ(hook_harness::invoke(entry), S_OK);
        EXPECT_EQ(mapping.controller->pumpWriteIndex, index + 1);
    }
    EXPECT_EQ(hook_harness::calls, 12u);
    EXPECT_EQ(prepared, 4u);
}

TEST(HookBuffer, ClosedCaptureDoesNotSkipRequiredServiceInstallation) {
    reset(3);
    void* vtable[5] = {nullptr, nullptr, nullptr, reinterpret_cast<void*>(&hook_harness::capGet),
                       reinterpret_cast<void*>(&hook_harness::capRelease)};
    struct Service { void** vtable; } service{vtable};
    serviceOutput = &service;
    g_getService = fakeGetService;
    g_capGet = nullptr;
    g_capRel = nullptr;
    void* output = nullptr;
    EXPECT_EQ(HookGetService(nullptr, __uuidof(IAudioCaptureClient), &output), S_OK);
    EXPECT_EQ(output, &service);
    EXPECT_EQ(vtable[3], reinterpret_cast<void*>(&HookCapGetBuffer));
    EXPECT_EQ(vtable[4], reinterpret_cast<void*>(&HookCapReleaseBuffer));
    EXPECT_EQ(g_capGet, &hook_harness::capGet);
    EXPECT_EQ(g_capRel, &hook_harness::capRelease);
    EXPECT_EQ(hook_harness::ring.writeIndex, 0u);
    g_getService = nullptr;
    serviceOutput = nullptr;
}
