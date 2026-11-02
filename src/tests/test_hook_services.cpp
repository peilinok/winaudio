#include <gtest/gtest.h>
#include <cstring>

#define DllMain HookServicesUnusedDllMain
#include "../hook/WinAudioHook.cpp"
#undef DllMain

namespace {
Ring serviceRing{};

// Let the SDK interface declarations determine the virtual method order.
class SimpleVolume : public ISimpleAudioVolume {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE SetMasterVolume(float value, LPCGUID ctx) override {
        ++calls;
        level = value;
        context = ctx;
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetMasterVolume(float* value) override {
        ++calls;
        *value = level;
        return result;
    }
    HRESULT STDMETHODCALLTYPE SetMute(BOOL value, LPCGUID ctx) override {
        ++calls;
        mute = value;
        context = ctx;
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetMute(BOOL* value) override {
        ++calls;
        *value = mute;
        return result;
    }
    unsigned calls = 0;
    float level = 0.25f;
    BOOL mute = FALSE;
    LPCGUID context = nullptr;
    HRESULT result = S_OK;
};

// Use a writable, object-local copy; never leave a test class's vtable patched.
class ServiceVtable {
public:
    explicit ServiceVtable(void* object) : object_(reinterpret_cast<void***>(object)), original_(*object_) {
        std::memcpy(slots_, original_, sizeof(slots_));
        *object_ = slots_;
    }
    ~ServiceVtable() { *object_ = original_; }
private:
    void*** object_;
    void** original_;
    void* slots_[7];
};

__declspec(noinline) HRESULT setVolume(ISimpleAudioVolume* volume, float value, LPCGUID context) {
    return volume->SetMasterVolume(value, context);
}
__declspec(noinline) HRESULT getVolume(ISimpleAudioVolume* volume, float* value) {
    return volume->GetMasterVolume(value);
}
__declspec(noinline) HRESULT setMute(ISimpleAudioVolume* volume, BOOL value, LPCGUID context) {
    return volume->SetMute(value, context);
}
__declspec(noinline) HRESULT getMute(ISimpleAudioVolume* volume, BOOL* value) {
    return volume->GetMute(value);
}

class HookServices : public ::testing::Test {
protected:
    void SetUp() override {
        std::memset(&serviceRing, 0, sizeof(serviceRing));
        serviceRing.captureEnabled = 1;
        g_ring = &serviceRing;
        InterlockedExchange(&g_quiet, 0);
        g_volGet = nullptr;
        g_volSet = nullptr;
        g_muteGet = nullptr;
        g_muteSet = nullptr;
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
        g_fxGet = nullptr;
        g_fxSet = nullptr;
#endif
    }
    void TearDown() override { g_ring = nullptr; }
    void expectCall(uint32_t index, const char* iface, const char* method, HRESULT hr) {
        ASSERT_GT(serviceRing.writeIndex, index);
        const auto& call = serviceRing.slots[index];
        EXPECT_STREQ(call.iface, iface);
        EXPECT_STREQ(call.method, method);
        EXPECT_EQ(call.hresult, hr);
    }
};

TEST_F(HookServices, MuteCallsUseSdkSignatures) {
    SimpleVolume volume;
    ServiceVtable table(&volume);
    patchService(&volume, __uuidof(ISimpleAudioVolume));
    const GUID context = {0x12345678, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};
    EXPECT_EQ(setMute(&volume, FALSE, nullptr), S_OK);
    ASSERT_EQ(serviceRing.writeIndex, 1u);
    // Check the safe FALSE call before exercising the original TRUE crash.
    ASSERT_STREQ(serviceRing.slots[0].method, "SetMute");
    EXPECT_EQ(setMute(&volume, TRUE, &context), S_OK);
    EXPECT_EQ(volume.mute, TRUE);
    EXPECT_EQ(volume.context, &context);
    BOOL mute = FALSE;
    EXPECT_EQ(getMute(&volume, &mute), S_OK);
    EXPECT_EQ(mute, TRUE);
    EXPECT_EQ(volume.calls, 3u);
    expectCall(1, "ISimpleAudioVolume", "SetMute", S_OK);
    expectCall(2, "ISimpleAudioVolume", "GetMute", S_OK);
}

TEST_F(HookServices, VolumeCallsUseSdkSignatures) {
    SimpleVolume volume;
    ServiceVtable table(&volume);
    patchService(&volume, __uuidof(ISimpleAudioVolume));
    const GUID context = {0x87654321, 0x4321, 0x8765, {8, 7, 6, 5, 4, 3, 2, 1}};
    EXPECT_EQ(setVolume(&volume, 0.625f, &context), S_OK);
    EXPECT_FLOAT_EQ(volume.level, 0.625f);
    EXPECT_EQ(volume.context, &context);
    float level = 0;
    EXPECT_EQ(getVolume(&volume, &level), S_OK);
    EXPECT_FLOAT_EQ(level, 0.625f);
    EXPECT_EQ(volume.calls, 2u);
    expectCall(0, "ISimpleAudioVolume", "SetMasterVolume", S_OK);
    expectCall(1, "ISimpleAudioVolume", "GetMasterVolume", S_OK);
}

TEST_F(HookServices, VolumeFailureAndCaptureDisabledPreserveForwarding) {
    SimpleVolume volume;
    ServiceVtable table(&volume);
    patchService(&volume, __uuidof(ISimpleAudioVolume));
    volume.result = E_ACCESSDENIED;
    EXPECT_EQ(setVolume(&volume, 0.5f, nullptr), E_ACCESSDENIED);
    float level = 0;
    EXPECT_EQ(getVolume(&volume, &level), E_ACCESSDENIED);
    EXPECT_EQ(setMute(&volume, TRUE, nullptr), E_ACCESSDENIED);
    BOOL mute = FALSE;
    EXPECT_EQ(getMute(&volume, &mute), E_ACCESSDENIED);
    expectCall(0, "ISimpleAudioVolume", "SetMasterVolume", E_ACCESSDENIED);
    expectCall(1, "ISimpleAudioVolume", "GetMasterVolume", E_ACCESSDENIED);
    expectCall(2, "ISimpleAudioVolume", "SetMute", E_ACCESSDENIED);
    expectCall(3, "ISimpleAudioVolume", "GetMute", E_ACCESSDENIED);
    serviceRing.captureEnabled = 0;
    volume.result = S_FALSE;
    EXPECT_EQ(setVolume(&volume, 0.375f, nullptr), S_FALSE);
    EXPECT_EQ(getVolume(&volume, &level), S_FALSE);
    EXPECT_FLOAT_EQ(level, 0.375f);
    EXPECT_EQ(setMute(&volume, FALSE, nullptr), S_FALSE);
    EXPECT_EQ(getMute(&volume, &mute), S_FALSE);
    EXPECT_EQ(mute, FALSE);
    EXPECT_EQ(volume.calls, 8u);
    EXPECT_EQ(serviceRing.writeIndex, 4u);
}

#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
class EffectsManager : public IAudioEffectsManager {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE RegisterAudioEffectsChangedNotificationCallback(
        IAudioEffectsChangedNotificationClient* client) override {
        ++registrations;
        callback = client;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE UnregisterAudioEffectsChangedNotificationCallback(
        IAudioEffectsChangedNotificationClient* client) override {
        ++unregistrations;
        callback = client;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetAudioEffects(AUDIO_EFFECT** out, UINT32* count) override {
        ++gets;
        *out = &effect;
        *count = 1;
        return result;
    }
    HRESULT STDMETHODCALLTYPE SetAudioEffectState(GUID id, AUDIO_EFFECT_STATE state) override {
        ++sets;
        effect.id = id;
        effect.state = state;
        return result;
    }
    unsigned registrations = 0, unregistrations = 0, gets = 0, sets = 0;
    IAudioEffectsChangedNotificationClient* callback = nullptr;
    AUDIO_EFFECT effect{};
    HRESULT result = S_OK;
};

__declspec(noinline) HRESULT registerEffects(IAudioEffectsManager* manager) {
    return manager->RegisterAudioEffectsChangedNotificationCallback(nullptr);
}
__declspec(noinline) HRESULT unregisterEffects(IAudioEffectsManager* manager) {
    return manager->UnregisterAudioEffectsChangedNotificationCallback(nullptr);
}
__declspec(noinline) HRESULT getEffects(IAudioEffectsManager* manager, AUDIO_EFFECT** effects, UINT32* count) {
    return manager->GetAudioEffects(effects, count);
}
__declspec(noinline) HRESULT setEffect(IAudioEffectsManager* manager, GUID id, AUDIO_EFFECT_STATE state) {
    return manager->SetAudioEffectState(id, state);
}

TEST_F(HookServices, EffectsNotificationsRemainUnhooked) {
    EffectsManager manager;
    ServiceVtable table(&manager);
    patchService(&manager, __uuidof(IAudioEffectsManager));
    EXPECT_EQ(registerEffects(&manager), E_NOTIMPL);
    EXPECT_EQ(manager.registrations, 1u);
    ASSERT_EQ(serviceRing.writeIndex, 0u);
    EXPECT_EQ(unregisterEffects(&manager), E_NOTIMPL);
    EXPECT_EQ(manager.unregistrations, 1u);
    EXPECT_EQ(serviceRing.writeIndex, 0u);
}

TEST_F(HookServices, EffectsCallsUseSdkSignatures) {
    EffectsManager manager;
    ServiceVtable table(&manager);
    patchService(&manager, __uuidof(IAudioEffectsManager));
    AUDIO_EFFECT* effects = nullptr;
    UINT32 count = 0;
    EXPECT_EQ(getEffects(&manager, &effects, &count), S_OK);
    EXPECT_EQ(effects, &manager.effect);
    EXPECT_EQ(count, 1u);
    const GUID id = {0x12345678, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};
    EXPECT_EQ(setEffect(&manager, id, AUDIO_EFFECT_STATE_ON), S_OK);
    EXPECT_TRUE(IsEqualGUID(manager.effect.id, id));
    EXPECT_EQ(manager.effect.state, AUDIO_EFFECT_STATE_ON);
    EXPECT_EQ(manager.gets, 1u);
    EXPECT_EQ(manager.sets, 1u);
    expectCall(0, "IAudioEffectsManager", "GetAudioEffects", S_OK);
    expectCall(1, "IAudioEffectsManager", "SetAudioEffectState", S_OK);
}

TEST_F(HookServices, EffectsFailureAndCaptureDisabledPreserveForwarding) {
    EffectsManager manager;
    ServiceVtable table(&manager);
    patchService(&manager, __uuidof(IAudioEffectsManager));
    manager.result = E_ACCESSDENIED;
    AUDIO_EFFECT* effects = nullptr;
    UINT32 count = 0;
    EXPECT_EQ(getEffects(&manager, &effects, &count), E_ACCESSDENIED);
    EXPECT_EQ(setEffect(&manager, GUID{}, AUDIO_EFFECT_STATE_OFF), E_ACCESSDENIED);
    expectCall(0, "IAudioEffectsManager", "GetAudioEffects", E_ACCESSDENIED);
    expectCall(1, "IAudioEffectsManager", "SetAudioEffectState", E_ACCESSDENIED);
    serviceRing.captureEnabled = 0;
    manager.result = S_FALSE;
    EXPECT_EQ(getEffects(&manager, &effects, &count), S_FALSE);
    EXPECT_EQ(effects, &manager.effect);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(setEffect(&manager, GUID{}, AUDIO_EFFECT_STATE_ON), S_FALSE);
    EXPECT_EQ(manager.effect.state, AUDIO_EFFECT_STATE_ON);
    EXPECT_EQ(manager.gets, 2u);
    EXPECT_EQ(manager.sets, 2u);
    EXPECT_EQ(serviceRing.writeIndex, 2u);
}
#endif
}  // namespace
