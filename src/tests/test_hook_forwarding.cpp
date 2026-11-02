#include <gtest/gtest.h>
#include <cstring>
#include <future>

namespace {
void observePatch(const char* stage, void** cell);
}
#define WA_HOOK_PATCH_OBSERVE(stage, cell) observePatch(#stage, cell)

#define DllMain HookForwardingUnusedDllMain
#include "../hook/WinAudioHook.cpp"
#undef DllMain
#undef WA_HOOK_PATCH_OBSERVE

namespace {
HANDLE pageOpen = nullptr, releasePage = nullptr, secondStarted = nullptr, secondProtected = nullptr;
volatile LONG installerCount = 0, activeProtection = 0, overlappingProtection = 0;
IAudioCaptureClient* publicationCapture = nullptr;
volatile LONG publicationForwarded = 0;
Ring forwardingRing{};
void observePatch(const char* stage, void**) {
    if (!pageOpen) return;
    if (!std::strcmp(stage, "before_lock") && InterlockedIncrement(&installerCount) == 2)
        SetEvent(secondStarted);
    if (!std::strcmp(stage, "protected_page")) {
        if (InterlockedIncrement(&activeProtection) != 1) InterlockedIncrement(&overlappingProtection);
        if (InterlockedCompareExchange(&installerCount, 0, 0) > 1) SetEvent(secondProtected);
        if (InterlockedCompareExchange(&installerCount, 0, 0) == 1) {
            SetEvent(pageOpen);
            WaitForSingleObject(releasePage, 5000);
        }
    }
    if (!std::strcmp(stage, "published") && publicationCapture) {
        UINT32 frames = 0;
        if (publicationCapture->GetBuffer(nullptr, &frames, nullptr, nullptr, nullptr) == S_OK && frames == 13)
            InterlockedIncrement(&publicationForwarded);
    }
    if (!std::strcmp(stage, "restored")) InterlockedDecrement(&activeProtection);
}
// The SDK supplies the ABI; different code bodies must never share an original.
template <unsigned Tag>
class CaptureImplementation final : public IAudioCaptureClient {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** data, UINT32* frames, DWORD* flags,
                                       UINT64* position, UINT64* qpc) override {
        ++calls;
        seenThis = this;
        if (data) *data = bytes;
        if (frames) *frames = Tag;
        if (flags) *flags = 0;
        if (position) *position = Tag * 10;
        if (qpc) *qpc = Tag * 100;
        return result;
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames) override {
        ++calls;
        seenThis = this;
        seenFrames = frames + Tag;
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32* frames) override {
        *frames = Tag;
        return result;
    }
    HRESULT result = S_OK;
    unsigned calls = 0;
    void* seenThis = nullptr;
    UINT32 seenFrames = 0;
    BYTE bytes[8]{};
};

template <size_t Count>
class LocalVtable {
public:
    explicit LocalVtable(void* object) : object_(static_cast<void***>(object)), saved_(*object_) {
        std::memcpy(slots_, saved_, sizeof(slots_));
        *object_ = slots_;
    }
    ~LocalVtable() { *object_ = saved_; }
private:
    void*** object_;
    void** saved_;
    void* slots_[Count];
};

__declspec(noinline) HRESULT getCapture(IAudioCaptureClient* capture, BYTE** data, UINT32* frames,
                                      DWORD* flags, UINT64* position, UINT64* qpc) {
    return capture->GetBuffer(data, frames, flags, position, qpc);
}
__declspec(noinline) HRESULT releaseCapture(IAudioCaptureClient* capture, UINT32 frames) {
    return capture->ReleaseBuffer(frames);
}

class HookForwarding : public testing::Test {
protected:
    void SetUp() override {
        g_ring = nullptr;
        g_capGet = nullptr;
        g_capRel = nullptr;
    }
    void TearDown() override { g_ring = nullptr; }
    template <unsigned First, unsigned Second>
    void checkOrder() {
        CaptureImplementation<First> first;
        CaptureImplementation<Second> second;
        LocalVtable<6> firstTable(&first), secondTable(&second);
        patchService(&first, __uuidof(IAudioCaptureClient));
        patchService(&second, __uuidof(IAudioCaptureClient));
        for (HRESULT hr : {S_OK, S_FALSE, E_FAIL}) {
            first.result = second.result = hr;
            checkCapture(first, First, hr);
            checkCapture(second, Second, hr);
        }
    }
    template <unsigned Tag>
    void checkCapture(CaptureImplementation<Tag>& capture, unsigned expectedTag, HRESULT hr) {
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 99;
        UINT64 position = 0, qpc = 0;
        const unsigned before = capture.calls;
        EXPECT_EQ(getCapture(&capture, &data, &frames, &flags, &position, &qpc), hr);
        EXPECT_EQ(data, capture.bytes);
        EXPECT_EQ(frames, expectedTag);
        EXPECT_EQ(position, expectedTag * 10);
        EXPECT_EQ(qpc, expectedTag * 100);
        EXPECT_EQ(capture.seenThis, &capture);
        EXPECT_EQ(releaseCapture(&capture, 73), hr);
        EXPECT_EQ(capture.seenFrames, 73 + expectedTag);
        EXPECT_EQ(capture.calls, before + 2);
    }
};

TEST_F(HookForwarding, DifferentImplementationsAThenBKeepTheirOwnApi) { checkOrder<13, 29>(); }
TEST_F(HookForwarding, DifferentImplementationsBThenAKeepTheirOwnApi) { checkOrder<29, 13>(); }

TEST_F(HookForwarding, AnAlreadyHookedCellWithoutAnOriginalIsRejectedWithoutRecursion) {
    CaptureImplementation<13> capture;
    LocalVtable<6> table(&capture);
    void* hook = reinterpret_cast<void*>(&HookCapGetBuffer);
    (*reinterpret_cast<void***>(&capture))[3] = hook;
    void* saved = nullptr;
    EXPECT_FALSE(wa::hook_ipc::patchVtableSlot(&capture, 3, hook, &saved));
    EXPECT_EQ(saved, nullptr);
    EXPECT_EQ(GetLastError(), static_cast<DWORD>(ERROR_INVALID_FUNCTION));
}

TEST_F(HookForwarding, LaterUnsupportedImplementationIsUnmodifiedAndReportsLimitedCoverage) {
    std::memset(&forwardingRing, 0, sizeof(forwardingRing));
    g_ring = &forwardingRing;
    CaptureImplementation<13> first;
    CaptureImplementation<29> second;
    LocalVtable<6> firstTable(&first), secondTable(&second);
    void* untouchedGet = (*reinterpret_cast<void***>(&second))[3];
    void* untouchedRelease = (*reinterpret_cast<void***>(&second))[4];
    patchService(&first, __uuidof(IAudioCaptureClient));
    EXPECT_EQ(forwardingRing.patchAccepted, 2u);
    EXPECT_EQ(forwardingRing.patchRejected, 0u);
    patchService(&second, __uuidof(IAudioCaptureClient));
    EXPECT_EQ(forwardingRing.patchRejected, 2u);
    EXPECT_EQ(forwardingRing.patchLastError, static_cast<DWORD>(ERROR_NOT_SUPPORTED));
    EXPECT_EQ(forwardingRing.coverageFlags, wa::hook_ipc::kCoverageDiscoveredImplementations);
    EXPECT_EQ((*reinterpret_cast<void***>(&second))[3], untouchedGet);
    EXPECT_EQ((*reinterpret_cast<void***>(&second))[4], untouchedRelease);
    checkCapture(first, 13, S_OK);
    checkCapture(second, 29, S_OK);
}

TEST_F(HookForwarding, FailedProtectionKeepsTheApiCallableAndReportsFailure) {
    std::memset(&forwardingRing, 0, sizeof(forwardingRing));
    g_ring = &forwardingRing;
    CaptureImplementation<13> installed;
    LocalVtable<6> installedTable(&installed);
    patchService(&installed, __uuidof(IAudioCaptureClient));
    CaptureImplementation<13> capture;
    void** original = *reinterpret_cast<void***>(&capture);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
    ASSERT_NE(map, nullptr);
    void* writable = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, 4096);
    ASSERT_NE(writable, nullptr);
    std::memcpy(writable, original, sizeof(void*) * 6);
    UnmapViewOfFile(writable);
    void* readonly = MapViewOfFile(map, FILE_MAP_READ, 0, 0, 4096);
    ASSERT_NE(readonly, nullptr);
    *reinterpret_cast<void***>(&capture) = static_cast<void**>(readonly);
    patchService(&capture, __uuidof(IAudioCaptureClient));
    EXPECT_EQ(forwardingRing.patchAccepted, 2u);
    EXPECT_EQ(forwardingRing.patchRejected, 2u);
    EXPECT_NE(forwardingRing.patchLastError, 0u);
    checkCapture(installed, 13, S_OK);
    checkCapture(capture, 13, S_OK);
    *reinterpret_cast<void***>(&capture) = original;
    UnmapViewOfFile(readonly);
    CloseHandle(map);
}

TEST_F(HookForwarding, InstallBalancesSuccessfulComInitializationAndPreservesExistingSta) {
    for (int initial : {0, 1, 2}) {
        const auto result = std::async(std::launch::async, [initial] {
            if (initial != 0) {
                const HRESULT initialized = CoInitializeEx(nullptr,
                    initial == 1 ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
                if (initialized != S_OK) return initialized;
            }
            install();
            if (initial == 1) CoUninitialize();
            const HRESULT other = CoInitializeEx(nullptr,
                initial == 2 ? COINIT_MULTITHREADED : COINIT_APARTMENTTHREADED);
            if (SUCCEEDED(other)) CoUninitialize();
            if (initial == 2) CoUninitialize();
            return other;
        }).get();
        EXPECT_EQ(result, initial == 2 ? RPC_E_CHANGED_MODE : S_OK) << initial;
    }
}

TEST_F(HookForwarding, RepeatedAndConcurrentInstallationPreservesThePageAndOriginal) {
    CaptureImplementation<13> first, second;
    void* page = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    ASSERT_NE(page, nullptr);
    void** firstOriginal = *reinterpret_cast<void***>(&first);
    void** secondOriginal = *reinterpret_cast<void***>(&second);
    std::memcpy(page, firstOriginal, sizeof(void*) * 6);
    *reinterpret_cast<void***>(&first) = static_cast<void**>(page);
    *reinterpret_cast<void***>(&second) = static_cast<void**>(page);
    DWORD old = 0;
    ASSERT_TRUE(VirtualProtect(page, 4096, PAGE_READONLY, &old));
    pageOpen = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    releasePage = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    secondStarted = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    secondProtected = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    publicationCapture = &first;
    publicationForwarded = 0;
    installerCount = activeProtection = overlappingProtection = 0;
    auto one = std::async(std::launch::async, [&] { patchService(&first, __uuidof(IAudioCaptureClient)); });
    const DWORD opened = WaitForSingleObject(pageOpen, 5000);
    EXPECT_EQ(opened, WAIT_OBJECT_0);
    auto two = std::async(std::launch::async, [&] { patchService(&second, __uuidof(IAudioCaptureClient)); });
    EXPECT_EQ(WaitForSingleObject(secondStarted, 5000), WAIT_OBJECT_0);
    EXPECT_EQ(WaitForSingleObject(secondProtected, 100), WAIT_TIMEOUT);
    SetEvent(releasePage);
    one.get();
    two.get();
    CloseHandle(pageOpen);
    CloseHandle(releasePage);
    CloseHandle(secondStarted);
    CloseHandle(secondProtected);
    pageOpen = releasePage = secondStarted = nullptr;
    secondProtected = nullptr;
    publicationCapture = nullptr;
    EXPECT_EQ(overlappingProtection, 0);
    EXPECT_GT(publicationForwarded, 0);
    checkCapture(first, 13, S_OK);
    checkCapture(second, 13, S_OK);
    patchService(&first, __uuidof(IAudioCaptureClient));
    MEMORY_BASIC_INFORMATION info{};
    ASSERT_NE(VirtualQuery(page, &info, sizeof(info)), 0u);
    EXPECT_EQ(info.Protect, static_cast<DWORD>(PAGE_READONLY));
    *reinterpret_cast<void***>(&first) = firstOriginal;
    *reinterpret_cast<void***>(&second) = secondOriginal;
    VirtualFree(page, 0, MEM_RELEASE);
}
}  // namespace
