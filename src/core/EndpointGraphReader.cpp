#include "EndpointGraphReader.h"
#include <cstdio>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <devicetopology.h>
#include <functiondiscoverykeys_devpkey.h>
#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "DeviceEnumerator.h"
#include "Log.h"

DEFINE_PROPERTYKEY(PKEY_FX_StreamEffectClsid,
                   0xd04e05a6, 0x594b, 0x4fb6, 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d, 5);
DEFINE_PROPERTYKEY(PKEY_FX_ModeEffectClsid,
                   0xd04e05a6, 0x594b, 0x4fb6, 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d, 6);
DEFINE_PROPERTYKEY(PKEY_FX_EndpointEffectClsid,
                   0xd04e05a6, 0x594b, 0x4fb6, 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d, 7);

namespace wa {
namespace {

std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

void readClsid(IPropertyStore* props, const PROPERTYKEY& key, const char* role,
               EndpointSnapshot& out) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    HRESULT hr = props->GetValue(key, &pv);
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetValue(FX CLSID)", role, wa::log::hrName(hr));
    if (SUCCEEDED(hr) && pv.vt == VT_LPWSTR && pv.pwszVal && pv.pwszVal[0]) {
        ApoSlot slot;
        slot.role = role;
        slot.clsid = wideToUtf8(pv.pwszVal);
        out.apos.push_back(std::move(slot));
    }
    PropVariantClear(&pv);
}

void readFxStore(IMMDevice* dev, EndpointSnapshot& out) {
    ComPtr<IPropertyStore> props;
    HRESULT hr = dev->OpenPropertyStore(STGM_READ, props.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "OpenPropertyStore", "STGM_READ",
           wa::log::hrName(hr));
    if (FAILED(hr) || !props) return;

    PROPVARIANT sysfx;
    PropVariantInit(&sysfx);
    hr = props->GetValue(PKEY_AudioEndpoint_Disable_SysFx, &sysfx);
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetValue(Disable_SysFx)", "",
           wa::log::hrName(hr));
    if (SUCCEEDED(hr) && (sysfx.vt == VT_UI4 || sysfx.vt == VT_BOOL || sysfx.vt == VT_I4)) {
        if (sysfx.vt == VT_BOOL)
            out.sysFxDisabled = sysfx.boolVal != VARIANT_FALSE;
        else
            out.sysFxDisabled = sysfx.ulVal != 0;
    }
    PropVariantClear(&sysfx);

    readClsid(props.Get(), PKEY_FX_StreamEffectClsid, "SFX", out);
    readClsid(props.Get(), PKEY_FX_ModeEffectClsid, "MFX", out);
    readClsid(props.Get(), PKEY_FX_EndpointEffectClsid, "EFX", out);
}

void addHardware(IPart* part, EndpointSnapshot& out) {
    LPWSTR name = nullptr;
    HRESULT hr = part->GetName(&name);
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "IPart::GetName", "", wa::log::hrName(hr));
    const std::string partName = SUCCEEDED(hr) && name ? wideToUtf8(name) : "part";
    if (name) CoTaskMemFree(name);

    ComPtr<IAudioMute> mute;
    hr = part->Activate(CLSCTX_ALL, __uuidof(IAudioMute), reinterpret_cast<void**>(mute.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "Activate(IAudioMute)", partName,
           wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Warn, "EndpointGraph", "Activate(IAudioMute)", partName,
               wa::log::hrName(hr));
    } else if (mute) {
        BOOL on = FALSE;
        hr = mute->GetMute(&on);
        WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetMute", partName, wa::log::hrName(hr));
        if (SUCCEEDED(hr))
            out.hardware.push_back(HardwareControl{"mute", on ? "true" : "false"});
    }

    ComPtr<IAudioVolumeLevel> vol;
    hr = part->Activate(CLSCTX_ALL, __uuidof(IAudioVolumeLevel),
                        reinterpret_cast<void**>(vol.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "Activate(IAudioVolumeLevel)", partName,
           wa::log::hrName(hr));
    if (SUCCEEDED(hr) && vol) {
        UINT nch = 0;
        hr = vol->GetChannelCount(&nch);
        WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetChannelCount", partName,
               wa::log::hrName(hr));
        if (SUCCEEDED(hr) && nch > 0) {
            float db = 0.f;
            hr = vol->GetLevel(0, &db);
            WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetLevel", partName,
                   wa::log::hrName(hr));
            if (SUCCEEDED(hr)) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.1f dB", static_cast<double>(db));
                out.hardware.push_back(HardwareControl{"volume", buf});
            }
        }
    }

    ComPtr<IAudioAutoGainControl> agc;
    hr = part->Activate(CLSCTX_ALL, __uuidof(IAudioAutoGainControl),
                        reinterpret_cast<void**>(agc.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "Activate(IAudioAutoGainControl)", partName,
           wa::log::hrName(hr));
    if (SUCCEEDED(hr) && agc) {
        BOOL on = FALSE;
        hr = agc->GetEnabled(&on);
        WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetEnabled(AGC)", partName,
               wa::log::hrName(hr));
        if (SUCCEEDED(hr))
            out.hardware.push_back(HardwareControl{"AGC", on ? "on" : "off"});
    }
}

void walkTopology(IDeviceTopology* topo, EndpointSnapshot& out) {
    UINT n = 0;
    HRESULT hr = topo->GetSubunitCount(&n);
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetSubunitCount",
           "n=" + std::to_string(n), wa::log::hrName(hr));
    if (FAILED(hr)) return;
    for (UINT i = 0; i < n; ++i) {
        ComPtr<ISubunit> sub;
        hr = topo->GetSubunit(i, sub.GetAddressOf());
        WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetSubunit",
               "i=" + std::to_string(i), wa::log::hrName(hr));
        if (FAILED(hr) || !sub) continue;
        ComPtr<IPart> part;
        hr = sub.As(&part);
        WA_LOG(wa::log::Level::Debug, "EndpointGraph", "QueryInterface(IPart)",
               "i=" + std::to_string(i), wa::log::hrName(hr));
        if (SUCCEEDED(hr) && part) addHardware(part.Get(), out);
    }
}

void readTopology(IMMDevice* dev, EndpointSnapshot& out) {
    ComPtr<IDeviceTopology> epTopo;
    HRESULT hr = dev->Activate(__uuidof(IDeviceTopology), CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(epTopo.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "Activate(IDeviceTopology)", "",
           wa::log::hrName(hr));
    if (FAILED(hr) || !epTopo) return;

    walkTopology(epTopo.Get(), out);

    UINT connectors = 0;
    hr = epTopo->GetConnectorCount(&connectors);
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetConnectorCount",
           "n=" + std::to_string(connectors), wa::log::hrName(hr));
    if (FAILED(hr) || connectors == 0) return;

    ComPtr<IConnector> conn;
    hr = epTopo->GetConnector(0, conn.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetConnector", "0", wa::log::hrName(hr));
    if (FAILED(hr) || !conn) return;

    ComPtr<IConnector> other;
    hr = conn->GetConnectedTo(other.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetConnectedTo", "", wa::log::hrName(hr));
    if (FAILED(hr) || !other) return;

    ComPtr<IPart> part;
    hr = other.As(&part);
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "QueryInterface(IPart)", "",
           wa::log::hrName(hr));
    if (FAILED(hr) || !part) {
        if (FAILED(hr))
            WA_LOG(wa::log::Level::Warn, "EndpointGraph", "QueryInterface(IPart)", "",
                   wa::log::hrName(hr));
        return;
    }

    ComPtr<IDeviceTopology> adapter;
    hr = part->GetTopologyObject(adapter.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "GetTopologyObject", "", wa::log::hrName(hr));
    if (SUCCEEDED(hr) && adapter) walkTopology(adapter.Get(), out);
}

}  // namespace

Result EndpointGraphReader::snapshot(DataFlow flow, const DeviceId& id, EndpointSnapshot& out) {
    out = EndpointSnapshot{};
    WA_LOG(wa::log::Level::Info, "EndpointGraph", "snapshot",
           "flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render") +
               " id=" + wa::narrowAscii(id),
           "start");

    ComInitGuard com;
    if (!com.ok()) return HrToResult(com.hr, "EndpointGraphReader: CoInitializeEx");

    DeviceEnumerator devices;
    AudioFormat mix{};
    Result r = devices.mixFormat(id, mix);
    if (r) out.mixFormat = formatAudio(mix);
    AudioFormat devFmt{};
    r = devices.deviceFormat(id, devFmt);
    if (r) out.deviceFormat = formatAudio(devFmt);
    AudioFormat oem{};
    r = devices.oemFormat(id, oem);
    if (r) out.oemFormat = formatAudio(oem);

    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "EndpointGraph", "CoCreateInstance(MMDeviceEnumerator)", "",
           wa::log::hrName(hr));
    if (FAILED(hr)) return HrToResult(hr, "EndpointGraphReader: CoCreateInstance");

    ComPtr<IMMDevice> dev;
    const EDataFlow ef = flow == DataFlow::Capture ? eCapture : eRender;
    hr = id.empty() ? e->GetDefaultAudioEndpoint(ef, eConsole, dev.GetAddressOf())
                    : e->GetDevice(id.c_str(), dev.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "EndpointGraph",
           id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice",
           wa::narrowAscii(id), wa::log::hrName(hr));
    if (FAILED(hr)) return HrToResult(hr, "EndpointGraphReader: GetDevice");

    readFxStore(dev.Get(), out);
    readTopology(dev.Get(), out);
    WA_LOG(wa::log::Level::Info, "EndpointGraph", "snapshot",
           "apos=" + std::to_string(out.apos.size()) +
               " hw=" + std::to_string(out.hardware.size()) +
               " sysfx=" + std::string(out.sysFxDisabled ? "off" : "on"),
           "ok");
    return Result::Ok();
}

}  // namespace wa
