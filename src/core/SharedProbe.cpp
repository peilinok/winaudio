#include "SharedProbe.h"
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
        std::vector<StreamAudioEffect> effects;
        Result r = readAudioEffectsFromClient(client_.Get(), effects);
        if (!r) return r;
        for (const auto& e : effects) {
            AdvertisedEffect a;
            a.typeName = e.typeName;
            a.on = e.state == AudioEffectState::On;
            a.canSetState = e.canSetState;
            out.push_back(std::move(a));
        }
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
