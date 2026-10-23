#include "EndpointEnhancements.h"
#include "ComUtil.h"
#include "DeviceEnumerator.h"
#include "PolicyConfig.h"
#include "Log.h"
#include "AudioFormatStr.h"
#include <mmdeviceapi.h>
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

Result endpointIdOf(IMMDevice* dev, DeviceId& out) {
    LPWSTR raw = nullptr;
    HRESULT hr = dev->GetId(&raw);
    if (FAILED(hr) || !raw) return HrToResult(hr, "EndpointEnhancements: GetId");
    out.assign(raw);
    CoTaskMemFree(raw);
    return Result::Ok();
}

}  // namespace

void noteAppStreamStarted() {
    g_appStreamActive.fetch_add(1, std::memory_order_acq_rel);
}

void noteAppStreamStopped() {
    const int prev = g_appStreamActive.fetch_sub(1, std::memory_order_acq_rel);
    if (prev <= 0) {
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

    DeviceId resolved = id;
    if (resolved.empty()) {
        if (Result r = endpointIdOf(dev.Get(), resolved); !r) {
            WA_LOG(wa::log::Level::Err, "EndpointFx", "GetId", "", r.message);
            return r;
        }
    }

    // Write FxProperties store (Sound CPL path) then endpoint store via IPolicyConfig.
    if (Result r = writeDisableSysFxUlVal(resolved, ulVal); !r) {
        WA_LOG(wa::log::Level::Err, "EndpointFx", "writeDisableSysFxUlVal", "", r.message);
        return r;
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