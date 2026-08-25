#include "SharedProbe.h"
#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "Log.h"
#include "StreamInit.h"
#include "WasapiStream.h"
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <cstring>

namespace wa {
namespace {

std::string guidToName(const GUID& id) {
    struct Row { GUID g; const char* name; };
    static const Row kRows[] = {
        {AUDIO_EFFECT_TYPE_ACOUSTIC_ECHO_CANCELLATION, "Acoustic Echo Cancellation"},
        {AUDIO_EFFECT_TYPE_NOISE_SUPPRESSION, "Noise Suppression"},
        {AUDIO_EFFECT_TYPE_AUTOMATIC_GAIN_CONTROL, "Automatic Gain Control"},
        {AUDIO_EFFECT_TYPE_BEAMFORMING, "Beam Forming"},
        {AUDIO_EFFECT_TYPE_CONSTANT_TONE_REMOVAL, "Constant Tone Removal"},
        {AUDIO_EFFECT_TYPE_EQUALIZER, "Equalizer"},
        {AUDIO_EFFECT_TYPE_LOUDNESS_EQUALIZER, "Loudness Equalizer"},
        {AUDIO_EFFECT_TYPE_BASS_BOOST, "Bass Boost"},
        {AUDIO_EFFECT_TYPE_VIRTUAL_SURROUND, "Virtual Surround"},
        {AUDIO_EFFECT_TYPE_VIRTUAL_HEADPHONES, "Virtual Headphones"},
        {AUDIO_EFFECT_TYPE_SPEAKER_FILL, "Speaker Fill"},
        {AUDIO_EFFECT_TYPE_ROOM_CORRECTION, "Room Correction"},
        {AUDIO_EFFECT_TYPE_BASS_MANAGEMENT, "Bass Management"},
        {AUDIO_EFFECT_TYPE_ENVIRONMENTAL_EFFECTS, "Environmental Effects"},
        {AUDIO_EFFECT_TYPE_SPEAKER_PROTECTION, "Speaker Protection"},
        {AUDIO_EFFECT_TYPE_SPEAKER_COMPENSATION, "Speaker Compensation"},
        {AUDIO_EFFECT_TYPE_DYNAMIC_RANGE_COMPRESSION, "Dynamic Range Compression"},
#ifdef AUDIO_EFFECT_TYPE_DEEP_NOISE_SUPPRESSION
        {AUDIO_EFFECT_TYPE_DEEP_NOISE_SUPPRESSION, "Deep Noise Suppression"},
#endif
    };
    for (const auto& row : kRows) {
        if (InlineIsEqualGUID(row.g, id)) return row.name;
    }
    wchar_t w[64] = {};
    const int n = StringFromGUID2(id, w, 64);
    if (n <= 1) return "Unknown effect";
    std::string s;
    s.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n - 1; ++i) s += static_cast<char>(w[i]);
    return s;
}

class WasapiSharedProbeHost : public SharedProbeHost {
public:
    WasapiSharedProbeHost(DataFlow flow, DeviceId id) : flow_(flow), id_(std::move(id)) {}

    Result open(const StreamParams& params) override {
        close();
        WA_LOG(wa::log::Level::Info, "SharedProbe", "open",
               std::string(flow_ == DataFlow::Capture ? "capture" : "render") +
                   " id=" + wa::narrowAscii(id_),
               "start");

        ComPtr<IMMDeviceEnumerator> e;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(e.GetAddressOf()));
        WA_LOG(wa::log::Level::Debug, "SharedProbe", "CoCreateInstance(MMDeviceEnumerator)", "",
               wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Err, "SharedProbe", "CoCreateInstance(MMDeviceEnumerator)", "",
                   wa::log::hrName(hr));
            return HrToResult(hr, "SharedProbe: CoCreateInstance");
        }

        const EDataFlow ef = flow_ == DataFlow::Capture ? eCapture : eRender;
        hr = id_.empty() ? e->GetDefaultAudioEndpoint(ef, eConsole, device_.GetAddressOf())
                         : e->GetDevice(id_.c_str(), device_.GetAddressOf());
        WA_LOG(wa::log::Level::Debug, "SharedProbe",
               id_.empty() ? "GetDefaultAudioEndpoint" : "GetDevice",
               wa::narrowAscii(id_), wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Err, "SharedProbe",
                   id_.empty() ? "GetDefaultAudioEndpoint" : "GetDevice",
                   wa::narrowAscii(id_), wa::log::hrName(hr));
            return HrToResult(hr, "SharedProbe: GetDevice");
        }

        hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(client_.GetAddressOf()));
        WA_LOG(wa::log::Level::Debug, "SharedProbe", "Activate(IAudioClient)", "",
               wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Err, "SharedProbe", "Activate(IAudioClient)", "",
                   wa::log::hrName(hr));
            return HrToResult(hr, "SharedProbe: Activate(IAudioClient)");
        }

        if (params.anyClientProps()) {
            AudioClientInitAdapter adapter(client_, device_.Get());
            AudioClientProperties p{};
            p.cbSize = sizeof(p);
            p.bIsOffload = params.clientProperties.offload ? TRUE : FALSE;
            p.eCategory = mapCategory(params.clientProperties.category);
            p.Options = mapStreamOption(params.clientProperties.option);
            hr = adapter.setClientProperties(p);
            if (FAILED(hr)) {
                WA_LOG(wa::log::Level::Err, "SharedProbe", "SetClientProperties", "",
                       wa::log::hrName(hr));
                return HrToResult(hr, "SharedProbe: SetClientProperties");
            }
        }

        AudioClientInitAdapter adapter(client_, device_.Get());
        StreamInitRequest req;
        req.params = params;
        req.direction = flow_ == DataFlow::Capture ? StreamInitDirection::Capture
                                                   : StreamInitDirection::Render;
        StreamInitOutcome out;
        Result r = streamInitShared(adapter, req, out);
        if (!r) return r;
        WA_LOG(wa::log::Level::Info, "SharedProbe", "Initialize(shared)",
               "fmt=" + wa::formatAudio(out.actualFormat) + " no Start", "ok");
        return Result::Ok();
    }

    Result readEffects(std::vector<AdvertisedEffect>& out) override {
        out.clear();
        if (!client_) return Result::Fail(-1, "SharedProbe: client not open");

        ComPtr<IAudioEffectsManager> fx;
        HRESULT hr = client_->GetService(__uuidof(IAudioEffectsManager),
                                         reinterpret_cast<void**>(fx.GetAddressOf()));
        WA_LOG(wa::log::Level::Debug, "SharedProbe", "GetService(IAudioEffectsManager)", "",
               wa::log::hrName(hr));
        if (FAILED(hr) || !fx) {
            WA_LOG(wa::log::Level::Warn, "SharedProbe", "GetService(IAudioEffectsManager)", "",
                   wa::log::hrName(FAILED(hr) ? hr : E_NOINTERFACE));
            return Result::Ok();
        }

        AUDIO_EFFECT* effects = nullptr;
        UINT32 n = 0;
        hr = fx->GetAudioEffects(&effects, &n);
        WA_LOG(wa::log::Level::Debug, "SharedProbe", "GetAudioEffects",
               "n=" + std::to_string(n), wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "SharedProbe", "GetAudioEffects", "", wa::log::hrName(hr));
            return Result::Ok();
        }
        for (UINT32 i = 0; i < n; ++i) {
            AdvertisedEffect a;
            a.typeName = guidToName(effects[i].id);
            a.on = effects[i].state == AUDIO_EFFECT_STATE_ON;
            a.canSetState = effects[i].canSetState != FALSE;
            out.push_back(std::move(a));
        }
        CoTaskMemFree(effects);
        return Result::Ok();
    }

    void close() override {
        if (client_) {
            WA_LOG(wa::log::Level::Info, "SharedProbe", "close", "", "ok");
        }
        client_.Reset();
        device_.Reset();
    }

private:
    DataFlow flow_;
    DeviceId id_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
};

}  // namespace

std::vector<SharedProbeRecipe> sharedProbeRecipes() {
    SharedProbeRecipe def;
    def.label = "Default";

    SharedProbeRecipe comm;
    comm.label = "Communications";
    comm.params.clientProperties.enabled = true;
    comm.params.clientProperties.category = AudioCategory::Communications;
    comm.params.clientProperties.option = StreamOption::None;

    SharedProbeRecipe raw;
    raw.label = "Raw";
    raw.raw = true;
    raw.params.clientProperties.enabled = true;
    raw.params.clientProperties.category = AudioCategory::Other;
    raw.params.clientProperties.option = StreamOption::Raw;

    return {def, comm, raw};
}

Result runSharedProbes(SharedProbeHost& host, bool exclusive, std::vector<ProbeSlice>& out) {
    if (exclusive) {
        WA_LOG(wa::log::Level::Warn, "SharedProbe", "runSharedProbes", "", "exclusive refused");
        return Result::Fail(-1, "exclusive probe refused");
    }
    out.clear();
    for (const auto& recipe : sharedProbeRecipes()) {
        ProbeSlice slice;
        slice.label = recipe.label;
        slice.raw = recipe.raw;
        Result r = host.open(recipe.params);
        if (r) {
            r = host.readEffects(slice.effects);
            if (!r) slice.error = r.message;
        } else {
            slice.error = r.message;
        }
        host.close();
        out.push_back(std::move(slice));
    }
    return Result::Ok();
}

Result probeEndpointShared(DataFlow flow, const DeviceId& id, std::vector<ProbeSlice>& out) {
    ComInitGuard com;
    if (!com.ok()) return HrToResult(com.hr, "SharedProbe: CoInitializeEx");
    WasapiSharedProbeHost host(flow, id);
    return runSharedProbes(host, false, out);
}

}  // namespace wa
