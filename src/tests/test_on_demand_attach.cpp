#include <gtest/gtest.h>
#include "HookedCallPod.h"
#include "OnDemandAttach.h"

using namespace wa;

TEST(OnDemandAttach, PidZeroFailsClosed) {
    EXPECT_EQ(evaluateAttach(0, 100, true, true, "chrome.exe"), AttachBlock::PidZero);
}

TEST(OnDemandAttach, SelfProcessFailsClosed) {
    EXPECT_EQ(evaluateAttach(100, 100, true, true, "WinAudioGui.exe"), AttachBlock::SelfProcess);
}

TEST(OnDemandAttach, AudiodgFailsClosed) {
    EXPECT_EQ(evaluateAttach(8, 100, true, true, "audiodg.exe"), AttachBlock::Audiodg);
    EXPECT_EQ(evaluateAttach(8, 100, true, true, "AUDIODG.EXE"), AttachBlock::Audiodg);
}

TEST(OnDemandAttach, CrossBitnessFailsClosed) {
    EXPECT_EQ(evaluateAttach(4242, 100, false, true, "chrome.exe"), AttachBlock::CrossBitness);
    EXPECT_STREQ(attachBlockText(AttachBlock::CrossBitness), "Attach failed: cross-bitness");
}

TEST(OnDemandAttach, MissingDebugRightsFailsClosed) {
    EXPECT_EQ(evaluateAttach(4242, 100, true, false, "chrome.exe"), AttachBlock::NoDebugRights);
    EXPECT_STREQ(attachBlockText(AttachBlock::NoDebugRights),
                 "Attach failed: missing debug rights");
}

TEST(OnDemandAttach, SameBitnessWithDebugIsAllowed) {
    EXPECT_EQ(evaluateAttach(4242, 100, true, true, "chrome.exe"), AttachBlock::None);
}

TEST(OnDemandAttach, DoesNotLaunchSuspendedOrAutoInject) {
    // Policy has no launch-suspended or broadcast-inject mode; start() is on-demand
    // for one PID. This test locks the public flags.
    EXPECT_FALSE(kAttachLaunchSuspended);
    EXPECT_FALSE(kAttachAutoInject);
}

TEST(OnDemandAttach, RemoteLoadLibraryNullModuleFailsClosed) {
    EXPECT_FALSE(remoteLoadLibrarySucceeded(0, 0));
}

TEST(OnDemandAttach, RemoteLoadLibraryTimeoutFailsClosed) {
    EXPECT_FALSE(remoteLoadLibrarySucceeded(258, 0x1000));
}

TEST(OnDemandAttach, RemoteLoadLibraryNonZeroModuleSucceeds) {
    EXPECT_TRUE(remoteLoadLibrarySucceeded(0, 0x00007FFC));
}

TEST(VtablePatch, ReplacesSlotAndKeepsOriginal) {
    void* origFn = reinterpret_cast<void*>(static_cast<uintptr_t>(0x1));
    void* hookFn = reinterpret_cast<void*>(static_cast<uintptr_t>(0x2));
    void* vt[4] = {nullptr, nullptr, nullptr, origFn};
    struct Obj {
        void** vtable;
    } obj{vt};
    void* saved = nullptr;
    ASSERT_TRUE(hook_ipc::patchVtableSlot(&obj, 3, hookFn, &saved));
    EXPECT_EQ(saved, origFn);
    EXPECT_EQ(vt[3], hookFn);
}

TEST(VtablePatch, IAudioClient3InitializeSharedSlotIs20) {
    EXPECT_EQ(hook_ipc::kSlotClientInitializeShared, 20);
    EXPECT_EQ(hook_ipc::kSlotClientGetService, 14);
    EXPECT_EQ(hook_ipc::kSlotBufferGetBuffer, 3);
}

TEST(OnDemandAttach, InstallFailMessageUsesSnapshotNotMappedRing) {
    EXPECT_STREQ(attachInstallFailMessage(hook_ipc::kInstallFailed),
                 "Attach: hook install failed in target");
    EXPECT_STREQ(attachInstallFailMessage(hook_ipc::kInstallPending),
                 "Attach: hook install did not finish; restart the target app and retry");
}

TEST(OnDemandAttach, SnapshotSurvivesUnmap) {
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
    ASSERT_TRUE(map);
    auto* view = static_cast<uint32_t*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 4096));
    ASSERT_TRUE(view);
    *view = hook_ipc::kInstallFailed;
    const uint32_t installed = *view;
    UnmapViewOfFile(view);
    CloseHandle(map);
    EXPECT_STREQ(attachInstallFailMessage(installed), "Attach: hook install failed in target");
}

TEST(OnDemandAttach, MapNameIncludesLayoutVersion) {
    wchar_t name[64] = {};
    hook_ipc::mapName(25060, name, 64);
    EXPECT_STREQ(name, L"Local\\WinAudioHook-2-25060");
}

TEST(OnDemandAttach, StagedHookFileNameFollowsLayout) {
    EXPECT_EQ(stagedHookFileName(), L"WinAudioHook-2.dll");
}

TEST(OnDemandAttach, CreateHookMappingIsVisibleInSameProcess) {
    const uint32_t pid = 424242u;
    HANDLE a = hook_ipc::createHookMapping(pid);
    ASSERT_TRUE(a);
    HANDLE b = hook_ipc::createHookMapping(pid);
    ASSERT_TRUE(b);
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_ALREADY_EXISTS));
    auto* ring = static_cast<hook_ipc::Ring*>(
        MapViewOfFile(b, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(hook_ipc::Ring)));
    ASSERT_TRUE(ring);
    ring->magic = hook_ipc::kMagic;
    UnmapViewOfFile(ring);
    CloseHandle(b);
    CloseHandle(a);
}
