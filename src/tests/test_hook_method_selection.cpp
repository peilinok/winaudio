#include <gtest/gtest.h>
#include <atomic>
#include <cstring>
#include <iterator>
#include <string>
#include <thread>

namespace {
void observeSelection(const char* event);
}
#define WA_HOOK_OBSERVE(event) observeSelection(#event)
#define DllMain HookMethodSelectionUnusedDllMain
#include "../hook/WinAudioHook.cpp"
#undef DllMain
#undef WA_HOOK_OBSERVE

namespace {
using Method = wa::MonitorMethod;
Ring selectionRing{};
unsigned prepared = 0, formatted = 0, packed = 0, timestamps = 0, writes = 0;
void observeSelection(const char* event) {
    if (!std::strcmp(event, "prepare")) ++prepared;
    if (!std::strcmp(event, "format")) ++formatted;
    if (!std::strcmp(event, "pack")) ++packed;
    if (!std::strcmp(event, "timestamp")) ++timestamps;
    if (!std::strcmp(event, "write")) ++writes;
}
void resetObservation(uint32_t methods, uint32_t generation = 7) {
    wa::hook_ipc::initializeRing(selectionRing);
    selectionRing.captureEnabled = 1;
    prepared = formatted = packed = timestamps = writes = 0;
    wa::hook_ipc::publishMonitorState(selectionRing, methods, generation);
}

// Every entry below is an SDK override, including x86 STDMETHODCALLTYPE.
// Shared fake state keeps argument checking independent of the Hook algorithms.
class AudioObjects : public IAudioClient3, public IMMDevice, public IAudioCaptureClient,
                     public IAudioRenderClient, public ISimpleAudioVolume,
                     public IAudioSessionControl, public IAudioClock, public IAudioStreamVolume
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
                     , public IAudioEffectsManager
#endif
{
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (iid == __uuidof(IAudioClient2)) *out = static_cast<IAudioClient2*>(this);
        else if (iid == __uuidof(IAudioClient3)) *out = static_cast<IAudioClient3*>(this);
        else return E_NOINTERFACE;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT finish(Method method) {
        seenThis = this;
        ++calls[static_cast<unsigned>(method)];
        if (entered) {
            SetEvent(entered);
            waitResult = WaitForSingleObject(resume, 5000);
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE mode, DWORD flags, REFERENCE_TIME duration,
        REFERENCE_TIME period, const WAVEFORMATEX* format, LPCGUID session) override {
        seenMode = mode; seenFlags = flags; seenDuration = duration; seenPeriod = period;
        seenFormat = format; seenContext = session;
        return finish(Method::Initialize);
    }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32* out) override {
        if (out) *out = 384;
        return finish(Method::GetBufferSize);
    }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Start() override { return finish(Method::Start); }
    HRESULT STDMETHODCALLTYPE Stop() override { return finish(Method::Stop); }
    HRESULT STDMETHODCALLTYPE Reset() override { return finish(Method::Reset); }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE event) override {
        seenEvent = event;
        return finish(Method::SetEventHandle);
    }
    HRESULT STDMETHODCALLTYPE GetService(REFIID iid, void** out) override {
        seenIid = iid;
        if (SUCCEEDED(result) && out) *out = serviceOutput;
        return finish(Method::GetService);
    }
    HRESULT STDMETHODCALLTYPE IsOffloadCapable(AUDIO_STREAM_CATEGORY, BOOL*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetClientProperties(const AudioClientProperties* properties) override {
        seenProperties = properties;
        return finish(Method::SetClientProperties);
    }
    HRESULT STDMETHODCALLTYPE GetBufferSizeLimits(const WAVEFORMATEX*, BOOL, REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetSharedModeEnginePeriod(const WAVEFORMATEX*, UINT32*, UINT32*, UINT32*, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentSharedModeEnginePeriod(WAVEFORMATEX**, UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InitializeSharedAudioStream(DWORD flags, UINT32 period,
        const WAVEFORMATEX* format, LPCGUID session) override {
        seenFlags = flags; seenFrames = period; seenFormat = format; seenContext = session;
        return finish(Method::InitializeSharedAudioStream);
    }
    HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD context, PROPVARIANT* params, void** out) override {
        seenIid = iid; seenFlags = context; seenActivation = params;
        if (SUCCEEDED(result) && out) *out = activationOutput;
        return finish(Method::Activate);
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetState(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBuffer(BYTE** data, UINT32* frames, DWORD* flags, UINT64* position, UINT64* qpc) override {
        if (SUCCEEDED(result)) {
            if (data) *data = bytes;
            if (frames) *frames = 137;
            if (flags) *flags = AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
            if (position) *position = 123456;
            if (qpc) *qpc = 654321;
        }
        return finish(Method::CaptureGetBuffer);
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames) override {
        seenFrames = frames;
        return finish(Method::CaptureReleaseBuffer);
    }
    HRESULT STDMETHODCALLTYPE GetNextPacketSize(UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT32 frames, BYTE** data) override {
        seenFrames = frames;
        if (data) *data = bytes;
        return finish(Method::RenderGetBuffer);
    }
    HRESULT STDMETHODCALLTYPE ReleaseBuffer(UINT32 frames, DWORD flags) override {
        seenFrames = frames; seenFlags = flags;
        return finish(Method::RenderReleaseBuffer);
    }
    HRESULT STDMETHODCALLTYPE SetMasterVolume(float level, LPCGUID context) override {
        seenLevel = level; seenContext = context;
        return finish(Method::SetMasterVolume);
    }
    HRESULT STDMETHODCALLTYPE GetMasterVolume(float* out) override {
        if (out) *out = 0.625f;
        return finish(Method::GetMasterVolume);
    }
    HRESULT STDMETHODCALLTYPE SetMute(BOOL mute, LPCGUID context) override {
        seenMute = mute; seenContext = context;
        return finish(Method::SetMute);
    }
    HRESULT STDMETHODCALLTYPE GetMute(BOOL* out) override {
        if (out) *out = TRUE;
        return finish(Method::GetMute);
    }
    HRESULT STDMETHODCALLTYPE GetState(AudioSessionState* out) override {
        if (out) *out = AudioSessionStateActive;
        return finish(Method::GetState);
    }
    HRESULT STDMETHODCALLTYPE GetDisplayName(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetDisplayName(LPCWSTR, LPCGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIconPath(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetIconPath(LPCWSTR, LPCGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGroupingParam(GUID*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetGroupingParam(LPCGUID, LPCGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterAudioSessionNotification(IAudioSessionEvents*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnregisterAudioSessionNotification(IAudioSessionEvents*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrequency(UINT64* out) override {
        if (out) *out = 48000;
        return finish(Method::GetFrequency);
    }
    HRESULT STDMETHODCALLTYPE GetPosition(UINT64*, UINT64*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetChannelCount(UINT32* out) override {
        if (out) *out = 6;
        return finish(Method::GetChannelCount);
    }
    HRESULT STDMETHODCALLTYPE SetChannelVolume(UINT32 channel, float level) override {
        seenChannel = channel; seenLevel = level;
        return finish(Method::SetChannelVolume);
    }
    HRESULT STDMETHODCALLTYPE GetChannelVolume(UINT32 channel, float* out) override {
        seenChannel = channel;
        if (out) *out = 0.375f;
        return finish(Method::GetChannelVolume);
    }
    HRESULT STDMETHODCALLTYPE SetAllVolumes(UINT32, const float*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAllVolumes(UINT32, float*) override { return E_NOTIMPL; }
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
    HRESULT STDMETHODCALLTYPE RegisterAudioEffectsChangedNotificationCallback(IAudioEffectsChangedNotificationClient*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnregisterAudioEffectsChangedNotificationCallback(IAudioEffectsChangedNotificationClient*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetAudioEffects(AUDIO_EFFECT** out, UINT32* count) override {
        if (out) *out = emptyEffects ? nullptr : &effect;
        if (count) *count = emptyEffects ? 0u : 1u;
        return finish(Method::GetAudioEffects);
    }
    HRESULT STDMETHODCALLTYPE SetAudioEffectState(GUID id, AUDIO_EFFECT_STATE state) override {
        effect.id = id; effect.state = state;
        return finish(Method::SetAudioEffectState);
    }
    AUDIO_EFFECT effect{};
    bool emptyEffects = false;
#endif
    unsigned calls[25]{};
    HRESULT result = S_OK;
    AudioObjects* seenThis = nullptr;
    HANDLE entered = nullptr, resume = nullptr;
    DWORD waitResult = WAIT_FAILED;
    AUDCLNT_SHAREMODE seenMode = AUDCLNT_SHAREMODE_SHARED;
    DWORD seenFlags = 0;
    REFERENCE_TIME seenDuration = 0, seenPeriod = 0;
    UINT32 seenFrames = 0, seenChannel = 0;
    const WAVEFORMATEX* seenFormat = nullptr;
    LPCGUID seenContext = nullptr;
    const AudioClientProperties* seenProperties = nullptr;
    PROPVARIANT* seenActivation = nullptr;
    HANDLE seenEvent = nullptr;
    GUID seenIid{};
    float seenLevel = 0;
    BOOL seenMute = FALSE;
    void* activationOutput = nullptr;
    void* serviceOutput = nullptr;
    BYTE bytes[8]{};
};

struct SdkObjects {
    IAudioClient3* client;
    IMMDevice* device;
    IAudioCaptureClient* capture;
    IAudioRenderClient* render;
    ISimpleAudioVolume* volume;
    IAudioSessionControl* session;
    IAudioClock* clock;
    IAudioStreamVolume* stream;
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
    IAudioEffectsManager* effects;
#endif
};
const GUID testContext = {0x12345678, 0x1234, 0x5678, {1, 2, 3, 4, 5, 6, 7, 8}};
const WAVEFORMATEX testFormat = {WAVE_FORMAT_PCM, 2, 48000, 192000, 4, 16, 0};
const AudioClientProperties testProperties = {sizeof(AudioClientProperties), FALSE,
    AudioCategory_Communications, AUDCLNT_STREAMOPTIONS_RAW};
PROPVARIANT testActivation{};

// The optimizer cannot infer a concrete class from these SDK pointers.
__declspec(noinline) HRESULT invokeMethod(SdkObjects& sdk, AudioObjects& expected,
                                         Method method, bool optionalNull = false) {
    const auto* context = optionalNull ? nullptr : &testContext;
    HRESULT hr = E_UNEXPECTED;
    UINT32 frames = 0, count = 0;
    DWORD flags = 0;
    UINT64 position = 0, qpc = 0, frequency = 0;
    BYTE* data = nullptr;
    void* object = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    float level = 0;
    BOOL mute = FALSE;
    AudioSessionState state = AudioSessionStateExpired;
    switch (method) {
    case Method::Activate:
        hr = sdk.device->Activate(__uuidof(IAudioClient3), CLSCTX_INPROC_SERVER,
            optionalNull ? nullptr : &testActivation, &object);
        EXPECT_EQ(object, SUCCEEDED(hr) ? expected.activationOutput
            : reinterpret_cast<void*>(static_cast<uintptr_t>(1)));
        EXPECT_TRUE(IsEqualGUID(expected.seenIid, __uuidof(IAudioClient3)));
        EXPECT_EQ(expected.seenFlags, static_cast<DWORD>(CLSCTX_INPROC_SERVER));
        EXPECT_EQ(expected.seenActivation, optionalNull ? nullptr : &testActivation);
        break;
    case Method::Initialize:
        hr = sdk.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            1234567, 7654321, &testFormat, context);
        EXPECT_EQ(expected.seenMode, AUDCLNT_SHAREMODE_EXCLUSIVE);
        EXPECT_EQ(expected.seenFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK));
        EXPECT_EQ(expected.seenDuration, 1234567);
        EXPECT_EQ(expected.seenPeriod, 7654321);
        EXPECT_EQ(expected.seenFormat, &testFormat);
        EXPECT_EQ(expected.seenContext, context);
        break;
    case Method::Start: return sdk.client->Start();
    case Method::Stop: return sdk.client->Stop();
    case Method::Reset: return sdk.client->Reset();
    case Method::SetEventHandle:
        hr = sdk.client->SetEventHandle(reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1234)));
        EXPECT_EQ(expected.seenEvent, reinterpret_cast<HANDLE>(static_cast<uintptr_t>(0x1234)));
        break;
    case Method::GetService:
        hr = sdk.client->GetService(__uuidof(ISimpleAudioVolume), &object);
        EXPECT_EQ(object, SUCCEEDED(hr) ? expected.serviceOutput
            : reinterpret_cast<void*>(static_cast<uintptr_t>(1)));
        EXPECT_TRUE(IsEqualGUID(expected.seenIid, __uuidof(ISimpleAudioVolume)));
        break;
    case Method::GetBufferSize:
        hr = sdk.client->GetBufferSize(&frames);
        EXPECT_EQ(frames, 384u);
        break;
    case Method::SetClientProperties:
        hr = sdk.client->SetClientProperties(&testProperties);
        EXPECT_EQ(expected.seenProperties, &testProperties);
        break;
    case Method::InitializeSharedAudioStream:
        hr = sdk.client->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            480, &testFormat, context);
        EXPECT_EQ(expected.seenFlags, static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK));
        EXPECT_EQ(expected.seenFrames, 480u);
        EXPECT_EQ(expected.seenFormat, &testFormat);
        EXPECT_EQ(expected.seenContext, context);
        break;
    case Method::CaptureGetBuffer:
        hr = sdk.capture->GetBuffer(&data, &frames, &flags,
            optionalNull ? nullptr : &position, optionalNull ? nullptr : &qpc);
        EXPECT_EQ(data, SUCCEEDED(hr) ? expected.bytes : nullptr);
        EXPECT_EQ(frames, SUCCEEDED(hr) ? 137u : 0u);
        EXPECT_EQ(flags, SUCCEEDED(hr) ? static_cast<DWORD>(AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) : 0u);
        EXPECT_EQ(position, optionalNull || FAILED(hr) ? 0u : 123456u);
        EXPECT_EQ(qpc, optionalNull || FAILED(hr) ? 0u : 654321u);
        break;
    case Method::CaptureReleaseBuffer:
        hr = sdk.capture->ReleaseBuffer(137);
        EXPECT_EQ(expected.seenFrames, 137u);
        break;
    case Method::RenderGetBuffer:
        hr = sdk.render->GetBuffer(139, &data);
        EXPECT_EQ(data, expected.bytes);
        EXPECT_EQ(expected.seenFrames, 139u);
        break;
    case Method::RenderReleaseBuffer:
        hr = sdk.render->ReleaseBuffer(141, AUDCLNT_BUFFERFLAGS_SILENT);
        EXPECT_EQ(expected.seenFrames, 141u);
        EXPECT_EQ(expected.seenFlags, static_cast<DWORD>(AUDCLNT_BUFFERFLAGS_SILENT));
        break;
    case Method::SetMasterVolume:
        hr = sdk.volume->SetMasterVolume(0.75f, context);
        EXPECT_FLOAT_EQ(expected.seenLevel, 0.75f);
        EXPECT_EQ(expected.seenContext, context);
        break;
    case Method::SetMute:
        hr = sdk.volume->SetMute(TRUE, context);
        EXPECT_EQ(expected.seenMute, TRUE);
        EXPECT_EQ(expected.seenContext, context);
        break;
    case Method::GetMasterVolume:
        hr = sdk.volume->GetMasterVolume(&level);
        EXPECT_FLOAT_EQ(level, 0.625f);
        break;
    case Method::GetMute:
        hr = sdk.volume->GetMute(&mute);
        EXPECT_EQ(mute, TRUE);
        break;
    case Method::GetState:
        hr = sdk.session->GetState(&state);
        EXPECT_EQ(state, AudioSessionStateActive);
        break;
    case Method::GetFrequency:
        hr = sdk.clock->GetFrequency(&frequency);
        EXPECT_EQ(frequency, 48000u);
        break;
    case Method::SetChannelVolume:
        hr = sdk.stream->SetChannelVolume(3, 0.875f);
        EXPECT_EQ(expected.seenChannel, 3u);
        EXPECT_FLOAT_EQ(expected.seenLevel, 0.875f);
        break;
    case Method::GetChannelCount:
        hr = sdk.stream->GetChannelCount(&count);
        EXPECT_EQ(count, 6u);
        break;
    case Method::GetChannelVolume:
        hr = sdk.stream->GetChannelVolume(4, &level);
        EXPECT_EQ(expected.seenChannel, 4u);
        EXPECT_FLOAT_EQ(level, 0.375f);
        break;
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
    case Method::GetAudioEffects: {
        AUDIO_EFFECT* effects = nullptr;
        hr = sdk.effects->GetAudioEffects(&effects, &count);
        EXPECT_EQ(effects, expected.emptyEffects ? nullptr : &expected.effect);
        EXPECT_EQ(count, expected.emptyEffects ? 0u : 1u);
        break;
    }
    case Method::SetAudioEffectState:
        hr = sdk.effects->SetAudioEffectState(testContext, AUDIO_EFFECT_STATE_ON);
        EXPECT_TRUE(IsEqualGUID(expected.effect.id, testContext));
        EXPECT_EQ(expected.effect.state, AUDIO_EFFECT_STATE_ON);
        break;
#endif
    default: ADD_FAILURE() << "SDK does not expose requested method"; break;
    }
    return hr;
}

__declspec(noinline) HRESULT invokeFailedOutput(SdkObjects& sdk, Method method,
    BYTE** data, UINT32* frames, DWORD* flags, void** object) {
    if (method == Method::CaptureGetBuffer)
        return sdk.capture->GetBuffer(data, frames, flags, nullptr, nullptr);
    if (method == Method::Activate)
        return sdk.device->Activate(__uuidof(IAudioClient3), CLSCTX_INPROC_SERVER, nullptr, object);
    return sdk.client->GetService(__uuidof(ISimpleAudioVolume), object);
}

struct MethodCase { Method id; const char* iface; const char* method; bool control; };
// An independent acceptance list prevents omissions/default mistakes in the product table
// from silently shrinking this test's coverage.
constexpr MethodCase methodCases[] = {
    {Method::Activate, "IMMDevice", "Activate", true},
    {Method::Initialize, "IAudioClient", "Initialize", true},
    {Method::Start, "IAudioClient", "Start", true},
    {Method::Stop, "IAudioClient", "Stop", true},
    {Method::Reset, "IAudioClient", "Reset", true},
    {Method::SetEventHandle, "IAudioClient", "SetEventHandle", true},
    {Method::GetService, "IAudioClient", "GetService", true},
    {Method::GetBufferSize, "IAudioClient", "GetBufferSize", false},
    {Method::SetClientProperties, "IAudioClient2", "SetClientProperties", true},
    {Method::InitializeSharedAudioStream, "IAudioClient3", "InitializeSharedAudioStream", true},
    {Method::CaptureGetBuffer, "IAudioCaptureClient", "GetBuffer", false},
    {Method::CaptureReleaseBuffer, "IAudioCaptureClient", "ReleaseBuffer", false},
    {Method::RenderGetBuffer, "IAudioRenderClient", "GetBuffer", false},
    {Method::RenderReleaseBuffer, "IAudioRenderClient", "ReleaseBuffer", false},
    {Method::SetMasterVolume, "ISimpleAudioVolume", "SetMasterVolume", true},
    {Method::SetMute, "ISimpleAudioVolume", "SetMute", true},
    {Method::GetMasterVolume, "ISimpleAudioVolume", "GetMasterVolume", false},
    {Method::GetMute, "ISimpleAudioVolume", "GetMute", false},
    {Method::GetState, "IAudioSessionControl", "GetState", false},
    {Method::GetFrequency, "IAudioClock", "GetFrequency", false},
    {Method::SetChannelVolume, "IAudioStreamVolume", "SetChannelVolume", true},
    {Method::GetChannelCount, "IAudioStreamVolume", "GetChannelCount", false},
    {Method::GetChannelVolume, "IAudioStreamVolume", "GetChannelVolume", false},
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
    {Method::SetAudioEffectState, "IAudioEffectsManager", "SetAudioEffectState", true},
    {Method::GetAudioEffects, "IAudioEffectsManager", "GetAudioEffects", false},
#endif
};

template <size_t Count>
class SelectionVtable {
public:
    explicit SelectionVtable(void* object) : object_(static_cast<void***>(object)), saved_(*object_) {
        std::memcpy(slots_, saved_, sizeof(slots_));
        *object_ = slots_;
    }
    ~SelectionVtable() { *object_ = saved_; }
private:
    void*** object_;
    void** saved_;
    void* slots_[Count];
};

class HookMethodSelection : public testing::Test {
protected:
    void SetUp() override {
        std::memset(&selectionRing, 0, sizeof(selectionRing));
        selectionRing.captureEnabled = 1;
        g_ring = &selectionRing;
        InterlockedExchange(&g_quiet, 0);
        resetObservation(0);
        patchClient(static_cast<IAudioClient3*>(&objects));
        patchDevice(static_cast<IMMDevice*>(&objects));
        patchService(static_cast<IAudioCaptureClient*>(&objects), __uuidof(IAudioCaptureClient));
        patchService(static_cast<IAudioRenderClient*>(&objects), __uuidof(IAudioRenderClient));
        patchService(static_cast<ISimpleAudioVolume*>(&objects), __uuidof(ISimpleAudioVolume));
        patchService(static_cast<IAudioSessionControl*>(&objects), __uuidof(IAudioSessionControl));
        patchService(static_cast<IAudioClock*>(&objects), __uuidof(IAudioClock));
        patchService(static_cast<IAudioStreamVolume*>(&objects), __uuidof(IAudioStreamVolume));
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
        patchService(static_cast<IAudioEffectsManager*>(&objects), __uuidof(IAudioEffectsManager));
#endif
    }
    void TearDown() override { g_ring = nullptr; }
    SdkObjects sdkObjects() {
        return {&objects, &objects, &objects, &objects, &objects, &objects, &objects, &objects
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
            , &objects
#endif
        };
    }
    void expectObservation(const MethodCase& method, HRESULT hr, bool enabled) {
        EXPECT_EQ(prepared, enabled ? 1u : 0u);
        EXPECT_EQ(formatted, enabled ? 1u : 0u);
        EXPECT_EQ(packed, enabled ? 1u : 0u);
        EXPECT_EQ(timestamps, enabled ? 1u : 0u);
        EXPECT_EQ(writes, enabled ? 1u : 0u);
        EXPECT_EQ(selectionRing.writeIndex, enabled && method.control ? 1u : 0u);
        EXPECT_EQ(selectionRing.pumpWriteIndex, enabled && !method.control ? 1u : 0u);
        if (!enabled) {
            EXPECT_EQ(selectionRing.pumpXruns, 0u);
            return;
        }
        const auto& pod = method.control ? selectionRing.slots[0] : selectionRing.pumpSlots[0];
        EXPECT_STREQ(pod.iface, method.iface);
        EXPECT_STREQ(pod.method, method.method);
        EXPECT_EQ(pod.hresult, hr);
        EXPECT_EQ(pod.observationGeneration, 7u);
        EXPECT_GT(pod.timeMs, 0);
    }
    AudioObjects objects;
    SelectionVtable<21> clientTable{static_cast<IAudioClient3*>(&objects)};
    SelectionVtable<7> deviceTable{static_cast<IMMDevice*>(&objects)};
    SelectionVtable<6> captureTable{static_cast<IAudioCaptureClient*>(&objects)};
    SelectionVtable<5> renderTable{static_cast<IAudioRenderClient*>(&objects)};
    SelectionVtable<7> volumeTable{static_cast<ISimpleAudioVolume*>(&objects)};
    SelectionVtable<12> sessionTable{static_cast<IAudioSessionControl*>(&objects)};
    SelectionVtable<6> clockTable{static_cast<IAudioClock*>(&objects)};
    SelectionVtable<8> streamTable{static_cast<IAudioStreamVolume*>(&objects)};
#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
    SelectionVtable<7> effectsTable{static_cast<IAudioEffectsManager*>(&objects)};
#endif
};

TEST_F(HookMethodSelection, DisabledStartStillForwardsWithoutObservationWork) {
    objects.result = S_FALSE;
    auto sdk = sdkObjects();
    EXPECT_EQ(invokeMethod(sdk, objects, Method::Start), S_FALSE);
    EXPECT_EQ(objects.calls[static_cast<unsigned>(Method::Start)], 1u);
    EXPECT_EQ(prepared + formatted + packed + timestamps + writes, 0u);
    EXPECT_EQ(selectionRing.writeIndex + selectionRing.pumpWriteIndex, 0u);
}

TEST_F(HookMethodSelection, WindowStatisticsCountCompletedCallsIndependentlyOfTransport) {
    auto sdk = sdkObjects();
    for (const auto& method : methodCases) {
        if (method.control) continue;
        SCOPED_TRACE(method.method);
        resetObservation(wa::methodBit(method.id));
        for (HRESULT hr : {S_OK, S_FALSE, E_ACCESSDENIED}) {
            objects.result = hr;
            EXPECT_EQ(invokeMethod(sdk, objects, method.id), hr);
        }
        wa::hook_ipc::publishMonitorState(selectionRing, 0, 7);
        EXPECT_EQ(invokeMethod(sdk, objects, method.id), E_ACCESSDENIED);
        wa::hook_ipc::publishMonitorState(selectionRing, wa::methodBit(method.id), 7);
        objects.result = S_OK;
        for (unsigned i = 0; i < 100; ++i) EXPECT_EQ(invokeMethod(sdk, objects, method.id), S_OK);
        const auto snapshot = wa::hook_ipc::readMethodStatistics(selectionRing, 7);
        ASSERT_TRUE(snapshot.known);
        const auto& counts = snapshot.methods[static_cast<size_t>(method.id)];
        EXPECT_FALSE(counts.unknown);
        EXPECT_EQ(counts.completed, 103u);
        EXPECT_EQ(counts.failed, 1u);
        EXPECT_EQ(counts.discontinuities, method.id == Method::CaptureGetBuffer ? 101u : 0u);
        EXPECT_GT(static_cast<uint32_t>(selectionRing.pumpCounters.overwritten), 0u);
    }
}

HRESULT emptyPacketResult = S_OK;
HRESULT STDMETHODCALLTYPE noPacket(IAudioCaptureClient*, BYTE**, UINT32*, DWORD*, UINT64*, UINT64*) {
    return emptyPacketResult;
}

TEST_F(HookMethodSelection, EmptyCaptureStatusesNeverReadUnproducedOutputsOrCountDiscontinuity) {
    resetObservation(wa::methodBit(Method::CaptureGetBuffer));
    const CapGetBufFn saved = g_capGet;
    g_capGet = noPacket;
    auto* invalidFrames = reinterpret_cast<UINT32*>(static_cast<uintptr_t>(1));
    auto* invalidFlags = reinterpret_cast<DWORD*>(static_cast<uintptr_t>(1));
    for (HRESULT hr : {S_FALSE, AUDCLNT_S_BUFFER_EMPTY, E_ACCESSDENIED}) {
        emptyPacketResult = hr;
        EXPECT_EQ(HookCapGetBuffer(nullptr, nullptr, invalidFrames, invalidFlags, nullptr, nullptr), hr);
    }
    emptyPacketResult = S_OK;
    UINT32 empty = 0;
    EXPECT_EQ(HookCapGetBuffer(nullptr, nullptr, &empty, invalidFlags, nullptr, nullptr), S_OK);
    g_capGet = saved;
    const auto snapshot = wa::hook_ipc::readMethodStatistics(selectionRing, 7);
    const auto& count = snapshot.methods[static_cast<size_t>(Method::CaptureGetBuffer)];
    EXPECT_EQ(count.completed, 4u);
    EXPECT_EQ(count.failed, 1u);
    EXPECT_EQ(count.discontinuities, 0u);
}

TEST_F(HookMethodSelection, EverySdkMethodPreservesForwardingAndHasZeroDisabledRecordWork) {
#if !defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
    ADD_FAILURE() << "The acceptance SDK must expose IAudioEffectsManager for all 25 methods";
#endif
    ASSERT_EQ(std::size(methodCases), 25u);
    auto sdk = sdkObjects();
    for (const auto& method : methodCases) {
        for (HRESULT hr : {S_OK, S_FALSE, E_ACCESSDENIED}) {
            for (bool optionalNull : {false, true}) {
                for (bool enabled : {true, false}) {
                    SCOPED_TRACE(testing::Message() << method.iface << "::" << method.method
                        << " hr=" << hr << " enabled=" << enabled << " optionalNull=" << optionalNull);
                    resetObservation(enabled ? wa::methodBit(method.id) : 0);
                    objects.result = hr;
                    const unsigned before = objects.calls[static_cast<unsigned>(method.id)];
                    unsigned totalBefore = 0;
                    for (unsigned calls : objects.calls) totalBefore += calls;
                    EXPECT_EQ(invokeMethod(sdk, objects, method.id, optionalNull), hr);
                    EXPECT_EQ(objects.seenThis, &objects);
                    EXPECT_EQ(objects.calls[static_cast<unsigned>(method.id)], before + 1);
                    unsigned totalAfter = 0;
                    for (unsigned calls : objects.calls) totalAfter += calls;
                    EXPECT_EQ(totalAfter, totalBefore + 1);
                    expectObservation(method, hr, enabled);
                }
            }
        }
    }
}

TEST_F(HookMethodSelection, DefaultsMatchIndependentControlAndQueryAcceptanceList) {
    auto sdk = sdkObjects();
    for (const auto& method : methodCases) {
        SCOPED_TRACE(testing::Message() << method.iface << "::" << method.method);
        resetObservation(wa::kDefaultMonitorMethods);
        EXPECT_EQ(invokeMethod(sdk, objects, method.id), S_OK);
        expectObservation(method, S_OK, method.control);
    }
}

TEST_F(HookMethodSelection, FailedApisDoNotRecordCallerSentinelsAsProducedOutputs) {
    auto sdk = sdkObjects();
    objects.result = E_ACCESSDENIED;
    for (Method method : {Method::CaptureGetBuffer, Method::Activate, Method::GetService}) {
        for (bool enabled : {true, false}) {
            SCOPED_TRACE(testing::Message() << static_cast<unsigned>(method) << " enabled=" << enabled);
            resetObservation(enabled ? wa::methodBit(method) : 0);
            auto* const sentinel = reinterpret_cast<void*>(static_cast<uintptr_t>(0x12345678));
            BYTE* data = static_cast<BYTE*>(sentinel);
            UINT32 frames = 0xDEADBEEF;
            DWORD flags = 0xABCDEF01;
            void* object = sentinel;
            const unsigned before = objects.calls[static_cast<unsigned>(method)];
            EXPECT_EQ(invokeFailedOutput(sdk, method, &data, &frames, &flags, &object), E_ACCESSDENIED);
            EXPECT_EQ(objects.calls[static_cast<unsigned>(method)], before + 1);
            EXPECT_EQ(data, sentinel);
            EXPECT_EQ(frames, 0xDEADBEEFu);
            EXPECT_EQ(flags, 0xABCDEF01u);
            EXPECT_EQ(object, sentinel);
            if (!enabled) {
                EXPECT_EQ(prepared + formatted + packed + timestamps + writes, 0u);
                EXPECT_EQ(selectionRing.writeIndex + selectionRing.pumpWriteIndex, 0u);
                continue;
            }
            if (method == Method::CaptureGetBuffer) {
                ASSERT_EQ(selectionRing.pumpWriteIndex, 1u);
                EXPECT_STREQ(selectionRing.pumpSlots[0].args, "frames=0 flags=0x0");
            } else {
                ASSERT_EQ(selectionRing.writeIndex, 1u);
                const auto& pod = selectionRing.slots[0];
                const char* outputName = method == Method::Activate ? "ppInterface" : "ppv";
                unsigned outputs = 0;
                ASSERT_TRUE(wa::hook_ipc::decodeArgBlob(pod.argBlob, pod.argBlobLen,
                    [&](uint8_t, uint8_t dir, const char*, uint8_t, const char* name, uint8_t nameLength,
                        const char*, uint8_t, const char* post, uint8_t postLength) {
                        if (dir != wa::hook_ipc::kArgOut || nameLength != std::strlen(outputName) ||
                            std::memcmp(name, outputName, nameLength)) return;
                        ++outputs;
                        EXPECT_EQ(std::string(post, postLength), "NULL");
                    }));
                EXPECT_EQ(outputs, 1u);
            }
        }
    }
}

#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
TEST_F(HookMethodSelection, EmptyEffectsOutputRemainsValidWhenSelectedOrClosed) {
    auto sdk = sdkObjects();
    objects.emptyEffects = true;
    for (HRESULT hr : {S_OK, S_FALSE}) {
        for (bool enabled : {true, false}) {
            resetObservation(enabled ? wa::methodBit(Method::GetAudioEffects) : 0);
            objects.result = hr;
            const unsigned before = objects.calls[static_cast<unsigned>(Method::GetAudioEffects)];
            EXPECT_EQ(invokeMethod(sdk, objects, Method::GetAudioEffects), hr);
            EXPECT_EQ(objects.calls[static_cast<unsigned>(Method::GetAudioEffects)], before + 1);
            expectObservation({Method::GetAudioEffects, "IAudioEffectsManager", "GetAudioEffects", false}, hr, enabled);
        }
    }
}
#endif

TEST_F(HookMethodSelection, DisabledDiscoveryStillHooksReturnedSdkObjects) {
    AudioObjects discovered;
    SelectionVtable<21> discoveredClient{static_cast<IAudioClient3*>(&discovered)};
    SelectionVtable<7> discoveredVolume{static_cast<ISimpleAudioVolume*>(&discovered)};
    objects.activationOutput = static_cast<IAudioClient3*>(&discovered);
    discovered.serviceOutput = static_cast<ISimpleAudioVolume*>(&discovered);
    auto sdk = sdkObjects();
    auto discoveredSdk = sdk;
    discoveredSdk.client = &discovered;
    discoveredSdk.volume = &discovered;
    const auto methods = wa::methodBit(Method::Start) | wa::methodBit(Method::SetMasterVolume);
    resetObservation(methods);
    EXPECT_EQ(invokeMethod(sdk, objects, Method::Activate), S_OK);
    EXPECT_EQ(objects.calls[static_cast<unsigned>(Method::Activate)], 1u);
    EXPECT_EQ(prepared + formatted + packed + timestamps + writes, 0u);
    EXPECT_EQ(invokeMethod(discoveredSdk, discovered, Method::GetService), S_OK);
    EXPECT_EQ(discovered.calls[static_cast<unsigned>(Method::GetService)], 1u);
    EXPECT_EQ(prepared + formatted + packed + timestamps + writes, 0u);
    EXPECT_EQ(invokeMethod(discoveredSdk, discovered, Method::Start), S_OK);
    EXPECT_EQ(invokeMethod(discoveredSdk, discovered, Method::SetMasterVolume), S_OK);
    ASSERT_EQ(selectionRing.writeIndex, 2u);
    EXPECT_EQ(discovered.calls[static_cast<unsigned>(Method::Start)], 1u);
    EXPECT_EQ(discovered.calls[static_cast<unsigned>(Method::SetMasterVolume)], 1u);
    EXPECT_STREQ(selectionRing.slots[0].method, "Start");
    EXPECT_STREQ(selectionRing.slots[1].method, "SetMasterVolume");
}

TEST_F(HookMethodSelection, SameNamedCaptureAndRenderMethodsRemainIndependent) {
    auto sdk = sdkObjects();
    for (Method selected : {Method::CaptureGetBuffer, Method::CaptureReleaseBuffer,
                            Method::RenderGetBuffer, Method::RenderReleaseBuffer}) {
        SCOPED_TRACE(static_cast<unsigned>(selected));
        resetObservation(wa::methodBit(selected));
        for (Method called : {Method::CaptureGetBuffer, Method::CaptureReleaseBuffer,
                              Method::RenderGetBuffer, Method::RenderReleaseBuffer}) {
            const unsigned before = objects.calls[static_cast<unsigned>(called)];
            EXPECT_EQ(invokeMethod(sdk, objects, called), S_OK);
            EXPECT_EQ(objects.calls[static_cast<unsigned>(called)], before + 1);
        }
        EXPECT_EQ(selectionRing.writeIndex, 0u);
        ASSERT_EQ(selectionRing.pumpWriteIndex, 1u);
        EXPECT_EQ(prepared, 1u);
        EXPECT_EQ(formatted, 1u);
        EXPECT_EQ(packed, 1u);
        EXPECT_EQ(timestamps, 1u);
        EXPECT_EQ(writes, 1u);
        const bool capture = selected == Method::CaptureGetBuffer || selected == Method::CaptureReleaseBuffer;
        const bool get = selected == Method::CaptureGetBuffer || selected == Method::RenderGetBuffer;
        EXPECT_STREQ(selectionRing.pumpSlots[0].iface, capture ? "IAudioCaptureClient" : "IAudioRenderClient");
        EXPECT_STREQ(selectionRing.pumpSlots[0].method, get ? "GetBuffer" : "ReleaseBuffer");
    }
}

TEST_F(HookMethodSelection, InFlightCallsKeepEntrySelectionAndCannotEnterAnotherGeneration) {
    auto sdk = sdkObjects();
    for (const auto& method : methodCases) {
        // 0: on->off, 1: off->on, 2: on->off->on, 3: old->new generation.
        for (unsigned transition = 0; transition < 4; ++transition) {
            SCOPED_TRACE(testing::Message() << method.iface << "::" << method.method
                << " transition=" << transition);
            const uint32_t bit = wa::methodBit(method.id);
            resetObservation(transition == 1 ? 0 : bit);
            objects.result = S_FALSE;
            objects.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            objects.resume = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            ASSERT_NE(objects.entered, nullptr);
            ASSERT_NE(objects.resume, nullptr);
            const unsigned before = objects.calls[static_cast<unsigned>(method.id)];
            HRESULT result = E_UNEXPECTED;
            std::thread original([&] { result = invokeMethod(sdk, objects, method.id); });
            const DWORD entered = WaitForSingleObject(objects.entered, 5000);
            EXPECT_EQ(entered, static_cast<DWORD>(WAIT_OBJECT_0));
            if (transition == 0) wa::hook_ipc::publishMonitorState(selectionRing, 0, 7);
            else if (transition == 1) wa::hook_ipc::publishMonitorState(selectionRing, bit, 7);
            else if (transition == 2) {
                wa::hook_ipc::publishMonitorState(selectionRing, 0, 7);
                wa::hook_ipc::publishMonitorState(selectionRing, bit, 7);
            } else wa::hook_ipc::publishMonitorState(selectionRing, bit, 8);
            SetEvent(objects.resume);
            original.join();
            EXPECT_EQ(objects.waitResult, static_cast<DWORD>(WAIT_OBJECT_0));
            CloseHandle(objects.entered);
            CloseHandle(objects.resume);
            objects.entered = objects.resume = nullptr;
            EXPECT_EQ(result, S_FALSE);
            EXPECT_EQ(objects.calls[static_cast<unsigned>(method.id)], before + 1);
            const bool recorded = transition == 0 || transition == 2;
            EXPECT_EQ(selectionRing.writeIndex + selectionRing.pumpWriteIndex, recorded ? 1u : 0u);
            EXPECT_EQ(writes, recorded ? 1u : 0u);
            EXPECT_EQ(timestamps, recorded ? 1u : 0u);
            const auto statistics = wa::hook_ipc::readMethodStatistics(selectionRing, transition == 3 ? 8 : 7);
            EXPECT_EQ(statistics.methods[static_cast<size_t>(method.id)].completed,
                recorded && !method.control ? 1u : 0u);
            if (recorded) {
                const auto& pod = method.control ? selectionRing.slots[0] : selectionRing.pumpSlots[0];
                EXPECT_EQ(pod.observationGeneration, 7u);
                EXPECT_STREQ(pod.iface, method.iface);
                EXPECT_STREQ(pod.method, method.method);
                EXPECT_EQ(pod.hresult, S_FALSE);
            }
            if (transition == 1) EXPECT_EQ(prepared + formatted + packed, 0u);
        }
    }
}

TEST_F(HookMethodSelection, ClosingAndReopeningPreservesExistingRowsWithoutBackfill) {
    auto sdk = sdkObjects();
    const auto bit = wa::methodBit(Method::SetMasterVolume);
    resetObservation(bit);
    EXPECT_EQ(invokeMethod(sdk, objects, Method::SetMasterVolume), S_OK);
    ASSERT_EQ(selectionRing.writeIndex, 1u);
    const CallPod first = selectionRing.slots[0];
    wa::hook_ipc::publishMonitorState(selectionRing, 0, 7);
    for (unsigned call = 0; call < 3; ++call)
        EXPECT_EQ(invokeMethod(sdk, objects, Method::SetMasterVolume), S_OK);
    wa::hook_ipc::publishMonitorState(selectionRing, bit, 7);
    wa::hook_ipc::publishMonitorState(selectionRing, bit, 7);
    EXPECT_EQ(selectionRing.writeIndex, 1u);
    EXPECT_EQ(invokeMethod(sdk, objects, Method::SetMasterVolume), S_OK);
    EXPECT_EQ(objects.calls[static_cast<unsigned>(Method::SetMasterVolume)], 5u);
    EXPECT_EQ(selectionRing.writeIndex, 2u);
    EXPECT_EQ(writes, 2u);
    EXPECT_EQ(std::memcmp(&first, &selectionRing.slots[0], sizeof(first)), 0);
}

TEST(HookMethodPublication, SharedMappingPublishesMaskAndGenerationAsOneSnapshot) {
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                       static_cast<DWORD>(sizeof(Ring)), nullptr);
    ASSERT_NE(mapping, nullptr);
    auto* published = static_cast<Ring*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring)));
    auto* observed = static_cast<Ring*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ring)));
    if (!published || !observed) {
        if (published) UnmapViewOfFile(published);
        if (observed) UnmapViewOfFile(observed);
        CloseHandle(mapping);
        FAIL() << "Cannot map both sides of the monitor-state boundary";
    }
    const uint32_t maskA = wa::methodBit(Method::Start) | wa::methodBit(Method::CaptureGetBuffer) |
                           wa::methodBit(Method::GetChannelCount);
    const uint32_t maskB = wa::methodBit(Method::Stop) | wa::methodBit(Method::RenderGetBuffer) |
                           wa::methodBit(Method::GetAudioEffects);
    const uint64_t expectedA = (uint64_t{0x12345678} << 32) | maskA;
    const uint64_t expectedB = (uint64_t{0x87654321} << 32) | maskB;
    wa::hook_ipc::publishMonitorState(*published, maskA, 0x12345678);
    std::atomic<bool> started{false}, done{false};
    std::thread writer([&] {
        while (!started.load(std::memory_order_acquire)) SwitchToThread();
        for (unsigned update = 0; update < 50000; ++update) {
            if (update & 1) wa::hook_ipc::publishMonitorState(*published, maskA, 0x12345678);
            else wa::hook_ipc::publishMonitorState(*published, maskB, 0x87654321);
            if (!(update % 64)) SwitchToThread();
        }
        done.store(true, std::memory_order_release);
    });
    unsigned reads = 0, mixed = 0;
    started.store(true, std::memory_order_release);
    do {
        const auto state = wa::hook_ipc::monitorState(*observed);
        if (state != expectedA && state != expectedB) ++mixed;
        ++reads;
    } while (!done.load(std::memory_order_acquire) || reads < 1000);
    writer.join();
    EXPECT_EQ(mixed, 0u);
    EXPECT_GE(reads, 1000u);
    UnmapViewOfFile(observed);
    UnmapViewOfFile(published);
    CloseHandle(mapping);
}
}  // namespace
