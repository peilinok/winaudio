#include "EndpointEnhancements.h"
#include "ComUtil.h"
#include "DeviceEnumerator.h"
#include "Log.h"
#include "AudioFormatStr.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <atomic>

namespace wa {
namespace {

std::atomic<int> g_appStreamActive{0};

EDataFlow toEDataFlow(DataFlow f) { return f == DataFlow::Capture ? eCapture : eRender; }

Result openEndpoint(DataFlow flow, const DeviceId& id, ComPtr<IMMDevice>& dev) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "EndpointFx", "CoCreateInstance(MMDeviceEnumerator)", "",
           wa::log::hrName(hr));
    if (FAILED(hr)) return HrToResult(hr, "EndpointEnhancements: CoCreateInstance");

    if (id.empty()) {
        hr = e->GetDefaultAudioEndpoint(toEDataFlow(flow), eConsole, dev.GetAddressOf());
        WA_LOG(wa::log::Level::Debug, "EndpointFx", "GetDefaultAudioEndpoint",
               flow == DataFlow::Capture ? "capture" : "render", wa::log::hrName(hr));
    } else {
        hr = e->GetDevice(id.c_str(), dev.GetAddressOf());
        WA_LOG(wa::log::Level::Debug, "EndpointFx", "GetDevice",
               wa::narrowAscii(id), wa::log::hrName(hr));
    }
    if (FAILED(hr)) {
        return HrToResult(hr, id.empty() ? "EndpointEnhancements: GetDefaultAudioEndpoint"
                                         : "EndpointEnhancements: GetDevice");
    }
    return Result::Ok();
}

}  // namespace

void noteAppStreamStarted() {
    g_appStreamActive.fetch_add(1, std::memory_order_acq_rel);
}

void noteAppStreamStopped() {
    const int prev = g_appStreamActive.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 0) {
        // Under-count (mismatched stop); clamp back to 0.
        g_appStreamActive.store(0, std::memory_order_release);
        WA_LOG(wa::log::Level::Warn, "EndpointFx", "noteAppStreamStopped", "",
               "underflow clamped");
    }
}

bool anyAppStreamActive() {
    return g_appStreamActive.load(std::memory_order_acquire) > 0;
}

Result setEndpointEnhancements(DataFlow flow, const DeviceId& id, EnhancementsState state,
                               DeviceInfo* outInfo) {
    uint32_t ulVal = 0;
    if (!enhancementsStateToSysFxUlVal(state, ulVal)) {
        return Result::Fail(-1, "EndpointEnhancements: state must be on or off");
    }
    if (anyAppStreamActive()) {
        WA_LOG(wa::log::Level::Warn, "EndpointFx", "setEndpointEnhancements", "",
               "refused: stream active");
        return refuseEnhancementsWhileStreaming();
    }

    ComInitGuard com;
    if (!com.ok()) return HrToResult(com.hr, "EndpointEnhancements: CoInitializeEx");

    WA_LOG(wa::log::Level::Info, "EndpointFx", "setEndpointEnhancements",
           std::string("flow=") + (flow == DataFlow::Capture ? "capture" : "render") +
               " state=" + enhancementsStateName(state) +
               " id=" + (id.empty() ? std::string("(default)") : wa::narrowAscii(id)),
           "start");

    ComPtr<IMMDevice> dev;
    if (Result r = openEndpoint(flow, id, dev); !r) {
        WA_LOG(wa::log::Level::Err, "EndpointFx", "openEndpoint", "", r.message);
        return r;
    }

    ComPtr<IPropertyStore> props;
    HRESULT hr = dev->OpenPropertyStore(STGM_READWRITE, props.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "EndpointFx", "OpenPropertyStore", "STGM_READWRITE",
           wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "EndpointFx", "OpenPropertyStore", "STGM_READWRITE",
               wa::log::hrName(hr));
        return HrToResult(hr, "EndpointEnhancements: OpenPropertyStore(STGM_READWRITE)");
    }

    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_UI4;
    pv.ulVal = ulVal;

    hr = props->SetValue(PKEY_AudioEndpoint_Disable_SysFx, pv);
    WA_LOG(wa::log::Level::Debug, "EndpointFx", "SetValue(Disable_SysFx)",
           "ulVal=" + std::to_string(ulVal), wa::log::hrName(hr));
    PropVariantClear(&pv);
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "EndpointFx", "SetValue(Disable_SysFx)", "",
               wa::log::hrName(hr));
        return HrToResult(hr, "EndpointEnhancements: SetValue(Disable_SysFx)");
    }

    hr = props->Commit();
    WA_LOG(wa::log::Level::Info, "EndpointFx", "Commit(Disable_SysFx)",
           "ulVal=" + std::to_string(ulVal), wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "EndpointFx", "Commit(Disable_SysFx)", "",
               wa::log::hrName(hr));
        return HrToResult(hr, "EndpointEnhancements: Commit");
    }

    if (outInfo) {
        DeviceEnumerator de;
        DeviceInfo refreshed{};
        Result rr = id.empty() ? de.defaultDevice(flow, refreshed)
                               : [&]() -> Result {
                                     std::vector<DeviceInfo> all;
                                     Result er = de.enumerate(flow, all);
                                     if (!er) return er;
                                     for (auto& d : all) {
                                         if (d.id == id) {
                                             refreshed = std::move(d);
                                             return Result::Ok();
                                         }
                                     }
                                     return Result::Fail(-1,
                                         "EndpointEnhancements: device not found after set");
                                 }();
        if (!rr) {
            WA_LOG(wa::log::Level::Warn, "EndpointFx", "refresh DeviceInfo", "", rr.message);
        } else {
            *outInfo = std::move(refreshed);
        }
    }

    WA_LOG(wa::log::Level::Info, "EndpointFx", "setEndpointEnhancements",
           enhancementsStateName(state), "ok");
    return Result::Ok();
}

}  // namespace wa
