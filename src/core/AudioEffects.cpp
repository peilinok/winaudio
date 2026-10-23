#include "AudioEffects.h"
#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "Log.h"
#include "StreamInit.h"
#include "WasapiStream.h"
#include <audioclient.h>
#include <mmdeviceapi.h>

namespace wa {
namespace {

// Hardcoded AUDIO_EFFECT_TYPE_* GUIDs (ksmedia.h) so older SDKs still map names.
struct EffectNameRow {
    GUID g;
    const char* name;
};

const EffectNameRow kEffectNames[] = {
    {{0x6f64adbe, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Acoustic Echo Cancellation"},
    {{0x6f64adbf, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Noise Suppression"},
    {{0x6f64adc0, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Automatic Gain Control"},
    {{0x6f64adc1, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Beam Forming"},
    {{0x6f64adc2, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Constant Tone Removal"},
    {{0x6f64adc3, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Equalizer"},
    {{0x6f64adc4, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Loudness Equalizer"},
    {{0x6f64adc5, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Bass Boost"},
    {{0x6f64adc6, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Virtual Surround"},
    {{0x6f64adc7, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Virtual Headphones"},
    {{0x6f64adc8, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Speaker Fill"},
    {{0x6f64adc9, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Room Correction"},
    {{0x6f64adca, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Bass Management"},
    {{0x6f64adcb, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Environmental Effects"},
    {{0x6f64adcc, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Speaker Protection"},
    {{0x6f64adcd, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Speaker Compensation"},
    {{0x6f64adce, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Dynamic Range Compression"},
    {{0x6f64adcf, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Far Field Beamforming"},
    {{0x6f64add0, 0x8211, 0x11e2, {0x8c, 0x70, 0x2c, 0x27, 0xd7, 0xf0, 0x01, 0xfa}},
     "Deep Noise Suppression"},
};

}  // namespace

std::string guidToBracedString(const GUID& id) {
    wchar_t w[64] = {};
    const int n = StringFromGUID2(id, w, 64);
    if (n <= 1) return {};
    std::string s;
    s.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n - 1; ++i) s += static_cast<char>(w[i]);
    return s;
}

std::string audioEffectTypeName(const GUID& id) {
    for (const auto& row : kEffectNames) {
        if (InlineIsEqualGUID(row.g, id)) return row.name;
    }
    const std::string braced = guidToBracedString(id);
    return braced.empty() ? "Unknown effect" : braced;
}

Result readAudioEffectsFromClient(IAudioClient* client, std::vector<StreamAudioEffect>& out) {
    out.clear();
    if (!client) return Result::Fail(-1, "AudioEffects: null client");

#if defined(__IAudioEffectsManager_INTERFACE_DEFINED__)
    ComPtr<IAudioEffectsManager> fx;
    HRESULT hr = client->GetService(__uuidof(IAudioEffectsManager),
                                    reinterpret_cast<void**>(fx.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "AudioEffects", "GetService(IAudioEffectsManager)", "",
           wa::log::hrName(hr));
    if (FAILED(hr) || !fx) {
        WA_LOG(wa::log::Level::Warn, "AudioEffects", "GetService(IAudioEffectsManager)", "",
               wa::log::hrName(FAILED(hr) ? hr : E_NOINTERFACE));
        // Not an error: Win10 / no stream FX manager → empty list.
        return Result::Ok();
    }

    AUDIO_EFFECT* effects = nullptr;
    UINT32 n = 0;
    hr = fx->GetAudioEffects(&effects, &n);
    WA_LOG(wa::log::Level::Debug, "AudioEffects", "GetAudioEffects",
           "n=" + std::to_string(n), wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Warn, "AudioEffects", "GetAudioEffects", "", wa::log::hrName(hr));
        return Result::Ok();
    }
    for (UINT32 i = 0; i < n; ++i) {
        StreamAudioEffect a;
        a.id = guidToBracedString(effects[i].id);
        a.typeName = audioEffectTypeName(effects[i].id);
        a.canSetState = effects[i].canSetState != FALSE;
        a.state = audioEffectStateFromWin32(static_cast<uint32_t>(effects[i].state));
        out.push_back(std::move(a));
    }
    CoTaskMemFree(effects);
#else
    WA_LOG(wa::log::Level::Warn, "AudioEffects", "IAudioEffectsManager", "",
           "SDK header unavailable");
    (void)client;
#endif
    return Result::Ok();
}

Result probeStreamAudioEffects(DataFlow flow, const DeviceId& id,
                               std::vector<StreamAudioEffect>& out,
                               const StreamParams& params) {
    out.clear();
    ComInitGuard com;
    if (!com.ok()) return HrToResult(com.hr, "AudioEffects: CoInitializeEx");

    WA_LOG(wa::log::Level::Info, "AudioEffects", "probeStreamAudioEffects",
           std::string(flow == DataFlow::Capture ? "capture" : "render") +
               " id=" + wa::narrowAscii(id),
           "start");

    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "AudioEffects", "CoCreateInstance(MMDeviceEnumerator)", "",
           wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "AudioEffects", "CoCreateInstance(MMDeviceEnumerator)", "",
               wa::log::hrName(hr));
        return HrToResult(hr, "AudioEffects: CoCreateInstance");
    }

    ComPtr<IMMDevice> device;
    const EDataFlow ef = flow == DataFlow::Capture ? eCapture : eRender;
    hr = id.empty() ? e->GetDefaultAudioEndpoint(ef, eConsole, device.GetAddressOf())
                    : e->GetDevice(id.c_str(), device.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "AudioEffects",
           id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice",
           wa::narrowAscii(id), wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "AudioEffects",
               id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice",
               wa::narrowAscii(id), wa::log::hrName(hr));
        return HrToResult(hr, "AudioEffects: GetDevice");
    }

    ComPtr<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(client.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "AudioEffects", "Activate(IAudioClient)", "",
           wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "AudioEffects", "Activate(IAudioClient)", "",
               wa::log::hrName(hr));
        return HrToResult(hr, "AudioEffects: Activate(IAudioClient)");
    }

    if (params.anyClientProps()) {
        AudioClientInitAdapter adapter(client, device.Get());
        AudioClientProperties p{};
        p.cbSize = sizeof(p);
        p.bIsOffload = params.clientProperties.offload ? TRUE : FALSE;
        p.eCategory = mapCategory(params.clientProperties.category);
        p.Options = mapStreamOption(params.clientProperties.option);
        hr = adapter.setClientProperties(p);
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Err, "AudioEffects", "SetClientProperties", "",
                   wa::log::hrName(hr));
            return HrToResult(hr, "AudioEffects: SetClientProperties");
        }
    }

    AudioClientInitAdapter adapter(client, device.Get());
    StreamInitRequest req;
    req.params = params;
    req.direction = flow == DataFlow::Capture ? StreamInitDirection::Capture
                                              : StreamInitDirection::Render;
    StreamInitOutcome outcome;
    Result r = streamInitShared(adapter, req, outcome);
    if (!r) return r;
    WA_LOG(wa::log::Level::Info, "AudioEffects", "Initialize(shared)",
           "fmt=" + wa::formatAudio(outcome.actualFormat) + " no Start", "ok");

    r = readAudioEffectsFromClient(client.Get(), out);
    WA_LOG(wa::log::Level::Info, "AudioEffects", "probeStreamAudioEffects",
           "effects=" + std::to_string(out.size()), r ? "ok" : r.message);
    return r;
}

}  // namespace wa
