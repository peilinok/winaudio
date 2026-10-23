#include "DeviceEnumerator.h"
#include "PolicyConfig.h"
#include "ComUtil.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include "Log.h"
#include "AudioFormatStr.h"


// FX / CompositeFX CLSID keys (audioenginebaseapo.h). Endpoint OpenPropertyStore
// often returns VT_EMPTY; INF values live under the MMDevices FxProperties registry key.
DEFINE_PROPERTYKEY(PKEY_FX_StreamEffectClsid,
                   0xd04e05a6, 0x594b, 0x4fb6, 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d, 5);
DEFINE_PROPERTYKEY(PKEY_FX_ModeEffectClsid,
                   0xd04e05a6, 0x594b, 0x4fb6, 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d, 6);
DEFINE_PROPERTYKEY(PKEY_FX_EndpointEffectClsid,
                   0xd04e05a6, 0x594b, 0x4fb6, 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d, 7);
DEFINE_PROPERTYKEY(PKEY_CompositeFX_StreamEffectClsid,
                   0xd04e05a6, 0x594b, 0x4fb6, 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d, 13);
DEFINE_PROPERTYKEY(PKEY_CompositeFX_ModeEffectClsid,
                   0xd04e05a6, 0x594b, 0x4fb6, 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d, 14);
DEFINE_PROPERTYKEY(PKEY_CompositeFX_EndpointEffectClsid,
                   0xd04e05a6, 0x594b, 0x4fb6, 0xa8, 0x0d, 0x01, 0xaf, 0x5e, 0xed, 0x7d, 0x1d, 15);
namespace wa {

namespace {
EDataFlow toEDataFlow(DataFlow f) { return f == DataFlow::Capture ? eCapture : eRender; }

std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// IMMDevice id is "{0.0.0.00000000}.{guid}" / "{0.0.1.00000000}.{guid}".
std::wstring endpointGuidKey(const DeviceId& id) {
    const auto pos = id.rfind(L'.');
    if (pos == std::wstring::npos || pos + 1 >= id.size()) return {};
    return id.substr(pos + 1);
}

void appendOption(std::vector<EnhancementOption>& out, const char* id, const char* name,
                  const std::string& value) {
    if (value.empty()) return;
    for (const auto& existing : out) {
        if (existing.id == id) return;
    }
    EnhancementOption opt;
    opt.id = id;
    opt.name = name;
    opt.value = value;
    out.push_back(std::move(opt));
}

std::string regMultiSzToCsv(const wchar_t* multi) {
    if (!multi) return {};
    std::string out;
    for (const wchar_t* p = multi; *p; p += wcslen(p) + 1) {
        if (!out.empty()) out += ',';
        out += wideToUtf8(p);
    }
    return out;
}

// FxProperties holds INF-published APO CLSIDs; not exposed via IMMDevice::OpenPropertyStore.
void readFxOptionsFromRegistry(DataFlow flow, const DeviceId& id,
                               std::vector<EnhancementOption>& out) {
    const std::wstring guid = endpointGuidKey(id);
    if (guid.empty()) return;
    const wchar_t* leaf = flow == DataFlow::Capture ? L"Capture" : L"Render";
    std::wstring path = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\Audio\\";
    path += leaf;
    path += L'\\';
    path += guid;
    path += L"\\FxProperties";

    HKEY key = nullptr;
    LONG lr = RegOpenKeyExW(HKEY_LOCAL_MACHINE, path.c_str(), 0, KEY_READ, &key);
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "RegOpenKeyEx(FxProperties)",
           wa::narrowAscii(guid), lr == ERROR_SUCCESS ? "OK" : ("err=" + std::to_string(lr)));
    if (lr != ERROR_SUCCESS || !key) return;

    auto readValue = [&](DWORD propId, const char* optId, const char* optName) {
        wchar_t valueName[128];
        swprintf_s(valueName, L"{d04e05a6-594b-4fb6-a80d-01af5eed7d1d},%u", propId);
        DWORD type = 0, bytes = 0;
        LONG q = RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &bytes);
        if (q != ERROR_SUCCESS || bytes == 0) return;
        std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 2, 0);
        q = RegQueryValueExW(key, valueName, nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf.data()), &bytes);
        if (q != ERROR_SUCCESS) return;
        std::string value;
        if (type == REG_SZ || type == REG_EXPAND_SZ) {
            value = wideToUtf8(buf.data());
        } else if (type == REG_MULTI_SZ) {
            value = regMultiSzToCsv(buf.data());
        }
        // Skip placeholder NIL CLSID-only entries.
        if (value == "{00000000-0000-0000-0000-000000000000}" ||
            value == "[{00000000-0000-0000-0000-000000000000}]") {
            return;
        }
        appendOption(out, optId, optName, value);
    };

    readValue(5,  "SFX",  "Stream effect (SFX)");
    readValue(6,  "MFX",  "Mode effect (MFX)");
    readValue(7,  "EFX",  "Endpoint effect (EFX)");
    readValue(13, "CSFX", "Composite stream effect");
    readValue(14, "CMFX", "Composite mode effect");
    readValue(15, "CEFX", "Composite endpoint effect");
    RegCloseKey(key);
}

void readEnhancements(IPropertyStore* props, DataFlow flow, const DeviceId& id, DeviceInfo& info) {
    info.enhancements = EnhancementsState::Unknown;
    info.enhancementOptions.clear();

    // Prefer Fx store (Sound CPL / Win11 Settings); fall back to endpoint OpenPropertyStore.
    if (auto ul = readDisableSysFxUlVal(id)) {
        info.enhancements = enhancementsStateFromSysFxUlVal(*ul);
        WA_LOG(wa::log::Level::Debug, "DeviceEnum", "readDisableSysFxUlVal",
               "ulVal=" + std::to_string(*ul),
               enhancementsStateName(info.enhancements));
    } else {
        PROPVARIANT sysfx;
        PropVariantInit(&sysfx);
        HRESULT hr = props->GetValue(PKEY_AudioEndpoint_Disable_SysFx, &sysfx);
        WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IPropertyStore::GetValue",
               "key=Disable_SysFx vt=" + std::to_string(static_cast<int>(sysfx.vt)),
               ::wa::log::hrName(hr));
        if (SUCCEEDED(hr)) {
            if (sysfx.vt == VT_UI4 || sysfx.vt == VT_I4 || sysfx.vt == VT_UINT || sysfx.vt == VT_INT) {
                info.enhancements = enhancementsStateFromSysFxUlVal(sysfx.ulVal);
            } else if (sysfx.vt == VT_BOOL) {
                // VARIANT_TRUE => disabled (mirrors EndpointGraphReader).
                info.enhancements = sysfx.boolVal != VARIANT_FALSE
                    ? EnhancementsState::Disabled : EnhancementsState::Enabled;
            } else if (sysfx.vt == VT_EMPTY || sysfx.vt == VT_NULL) {
                // Key unset: Windows treats system effects as enabled by default.
                info.enhancements = EnhancementsState::Enabled;
            }
        }
        PropVariantClear(&sysfx);
    }

    auto readClsid = [&](const PROPERTYKEY& key, const char* optId, const char* optName) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        HRESULT h = props->GetValue(key, &pv);
        WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IPropertyStore::GetValue",
               std::string("key=FX_") + optId + " vt=" + std::to_string(static_cast<int>(pv.vt)),
               ::wa::log::hrName(h));
        if (SUCCEEDED(h) && (pv.vt == VT_LPWSTR || pv.vt == VT_BSTR) && pv.pwszVal && pv.pwszVal[0]) {
            appendOption(info.enhancementOptions, optId, optName, wideToUtf8(pv.pwszVal));
        }
        PropVariantClear(&pv);
    };
    readClsid(PKEY_FX_StreamEffectClsid,            "SFX",  "Stream effect (SFX)");
    readClsid(PKEY_FX_ModeEffectClsid,              "MFX",  "Mode effect (MFX)");
    readClsid(PKEY_FX_EndpointEffectClsid,          "EFX",  "Endpoint effect (EFX)");
    readClsid(PKEY_CompositeFX_StreamEffectClsid,   "CSFX", "Composite stream effect");
    readClsid(PKEY_CompositeFX_ModeEffectClsid,     "CMFX", "Composite mode effect");
    readClsid(PKEY_CompositeFX_EndpointEffectClsid, "CEFX", "Composite endpoint effect");

    if (info.enhancementOptions.empty())
        readFxOptionsFromRegistry(flow, id, info.enhancementOptions);
}
bool readFormatKey(IPropertyStore* props, const PROPERTYKEY& key, AudioFormat& out) {
    PROPVARIANT pv; PropVariantInit(&pv);
    bool ok = false;
    HRESULT gvHr = props->GetValue(key, &pv);
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IPropertyStore::GetValue", "key=format", ::wa::log::hrName(gvHr));
    if (SUCCEEDED(gvHr) && pv.vt == VT_BLOB &&
        pv.blob.cbSize >= sizeof(WAVEFORMATEX) && pv.blob.pBlobData != nullptr) {
        const auto* wf = reinterpret_cast<const WAVEFORMATEX*>(pv.blob.pBlobData);
        if (wf->wFormatTag != WAVE_FORMAT_EXTENSIBLE ||
            pv.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
            out = fromWaveFormat(wf);
            ok = true;
        }
    }
    PropVariantClear(&pv);
    return ok;
}

Result openDevice(DataFlow flow, const DeviceId& id, ComPtr<IMMDevice>& dev) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(e.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "CoCreateInstance", "clsid=MMDeviceEnumerator", ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "CoCreateInstance", "clsid=MMDeviceEnumerator", ::wa::log::hrName(hr));
        return HrToResult(hr, "CoCreateInstance");
    }
    const EDataFlow ef = (flow == DataFlow::Capture) ? eCapture : eRender;
    hr = id.empty() ? e->GetDefaultAudioEndpoint(ef, eConsole, dev.GetAddressOf())
                    : e->GetDevice(id.c_str(), dev.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "DeviceEnum",
           id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice",
           id.empty() ? "flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render")
                      : "id=" + wa::narrowAscii(id),
           ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum",
               id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice",
               id.empty() ? "flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render")
                          : "id=" + wa::narrowAscii(id),
               ::wa::log::hrName(hr));
        return HrToResult(hr, id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice");
    }
    return Result::Ok();
}

Result readInfo(IMMDevice* dev, DataFlow flow, bool isDefault, DeviceInfo& info) {
    LPWSTR idStr = nullptr;
    HRESULT hr = dev->GetId(&idStr);
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IMMDevice::GetId", "", ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "IMMDevice::GetId", "", ::wa::log::hrName(hr));
        return HrToResult(hr, "IMMDevice::GetId");
    }
    info.id = idStr;
    CoTaskMemFree(idStr);
    info.flow = flow;
    info.isDefault = isDefault;

    ComPtr<IPropertyStore> props;
    HRESULT opsHr = dev->OpenPropertyStore(STGM_READ, &props);
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IMMDevice::OpenPropertyStore", "mode=STGM_READ", ::wa::log::hrName(opsHr));
    if (SUCCEEDED(opsHr)) {
        PROPVARIANT name; PropVariantInit(&name);
        HRESULT fnHr = props->GetValue(PKEY_Device_FriendlyName, &name);
        WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IPropertyStore::GetValue", "key=FriendlyName", ::wa::log::hrName(fnHr));
        if (SUCCEEDED(fnHr) &&
            name.vt == VT_LPWSTR)
            info.name = name.pwszVal;
        PropVariantClear(&name);
        info.hasDeviceFormat = readFormatKey(props.Get(), PKEY_AudioEngine_DeviceFormat, info.deviceFormat);
        info.hasOemFormat    = readFormatKey(props.Get(), PKEY_AudioEngine_OEMFormat,    info.oemFormat);
        readEnhancements(props.Get(), flow, info.id, info);
    }

    ComPtr<IAudioClient> client;
    HRESULT actHr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                  reinterpret_cast<void**>(client.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IMMDevice::Activate", "iid=IAudioClient", ::wa::log::hrName(actHr));
    if (SUCCEEDED(actHr)) {
        WAVEFORMATEX* mix = nullptr;
        HRESULT gmHr = client->GetMixFormat(&mix);
        WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IAudioClient::GetMixFormat", "", ::wa::log::hrName(gmHr));
        if (SUCCEEDED(gmHr) && mix) {
            info.mixFormat = fromWaveFormat(mix);
            CoTaskMemFree(mix);
        }
    }
    return Result::Ok();
}
} // namespace

Result DeviceEnumerator::enumerate(DataFlow flow, std::vector<DeviceInfo>& out) {
    out.clear();
    WA_LOG(wa::log::Level::Info, "DeviceEnum", "enumerate",
           "flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render"), "start");
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "CoCreateInstance", "clsid=MMDeviceEnumerator", ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "CoCreateInstance", "clsid=MMDeviceEnumerator", ::wa::log::hrName(hr));
        return HrToResult(hr, "CoCreateInstance(MMDeviceEnumerator)");
    }

    DeviceId defaultId;
    {
        ComPtr<IMMDevice> def;
        HRESULT defHr = e->GetDefaultAudioEndpoint(toEDataFlow(flow), eConsole, &def);
        WA_LOG(wa::log::Level::Debug, "DeviceEnum", "GetDefaultAudioEndpoint",
               "flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render"), ::wa::log::hrName(defHr));
        if (SUCCEEDED(defHr)) {
            LPWSTR s = nullptr;
            HRESULT idHr = def->GetId(&s);
            WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IMMDevice::GetId", "default", ::wa::log::hrName(idHr));
            if (SUCCEEDED(idHr)) {
                defaultId = s; CoTaskMemFree(s);
                WA_LOG(wa::log::Level::Info, "DeviceEnum", "GetDefaultAudioEndpoint", "defaultId", "selected");
            }
        }
    }

    ComPtr<IMMDeviceCollection> coll;
    hr = e->EnumAudioEndpoints(toEDataFlow(flow), DEVICE_STATE_ACTIVE, &coll);
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "EnumAudioEndpoints",
           "flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render") + " state=ACTIVE",
           ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "EnumAudioEndpoints", "", ::wa::log::hrName(hr));
        return HrToResult(hr, "EnumAudioEndpoints");
    }

    UINT count = 0;
    {
        HRESULT gcHr = coll->GetCount(&count);
        WA_LOG(wa::log::Level::Warn, "DeviceEnum", "IMMDeviceCollection::GetCount",
               "count=" + std::to_string(count), ::wa::log::hrName(gcHr));
    }
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        HRESULT itemHr = coll->Item(i, &dev);
        WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IMMDeviceCollection::Item",
               "i=" + std::to_string(i), ::wa::log::hrName(itemHr));
        if (FAILED(itemHr)) continue;
        DeviceInfo info{};
        if (readInfo(dev.Get(), flow, false, info)) {
            info.isDefault = (info.id == defaultId);
            out.push_back(std::move(info));
        }
    }
    return Result::Ok();
}

Result DeviceEnumerator::defaultDevice(DataFlow flow, DeviceInfo& out) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "CoCreateInstance", "clsid=MMDeviceEnumerator", ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "CoCreateInstance", "clsid=MMDeviceEnumerator", ::wa::log::hrName(hr));
        return HrToResult(hr, "CoCreateInstance(MMDeviceEnumerator)");
    }
    ComPtr<IMMDevice> dev;
    hr = e->GetDefaultAudioEndpoint(toEDataFlow(flow), eConsole, &dev);
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "GetDefaultAudioEndpoint",
           "flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render"), ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "GetDefaultAudioEndpoint",
               "flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render"), ::wa::log::hrName(hr));
        return HrToResult(hr, "GetDefaultAudioEndpoint");
    }
    Result r = readInfo(dev.Get(), flow, true, out);
    if (r) WA_LOG(wa::log::Level::Info, "DeviceEnum", "defaultDevice",
                  "flow=" + std::string(flow == DataFlow::Capture ? "capture" : "render"), "ok");
    return r;
}

Result DeviceEnumerator::mixFormat(const DeviceId& id, AudioFormat& out) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "CoCreateInstance", "clsid=MMDeviceEnumerator", ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "CoCreateInstance", "clsid=MMDeviceEnumerator", ::wa::log::hrName(hr));
        return HrToResult(hr, "CoCreateInstance(MMDeviceEnumerator)");
    }
    ComPtr<IMMDevice> dev;
    if (id.empty())
        hr = e->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    else
        hr = e->GetDevice(id.c_str(), &dev);
    WA_LOG(wa::log::Level::Debug, "DeviceEnum",
           id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice",
           id.empty() ? std::string("flow=render") : "id=" + wa::narrowAscii(id),
           ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum",
               id.empty() ? "GetDefaultAudioEndpoint" : "GetDevice",
               id.empty() ? std::string("flow=render") : "id=" + wa::narrowAscii(id),
               ::wa::log::hrName(hr));
        return HrToResult(hr, "GetDevice");
    }

    ComPtr<IAudioClient> client;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                       reinterpret_cast<void**>(client.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IMMDevice::Activate", "iid=IAudioClient", ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "IMMDevice::Activate", "iid=IAudioClient", ::wa::log::hrName(hr));
        return HrToResult(hr, "IMMDevice::Activate");
    }
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IAudioClient::GetMixFormat", "", ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "IAudioClient::GetMixFormat", "", ::wa::log::hrName(hr));
        return HrToResult(hr, "GetMixFormat");
    }
    out = fromWaveFormat(mix);
    CoTaskMemFree(mix);
    return Result::Ok();
}

Result DeviceEnumerator::deviceFormat(const DeviceId& id, AudioFormat& out) {
    ComInitGuard com;
    if (!com.ok()) return HrToResult(com.hr, "DeviceEnumerator: CoInitializeEx");
    ComPtr<IMMDevice> dev;
    if (Result r = openDevice(DataFlow::Render, id, dev); !r) return r;
    ComPtr<IPropertyStore> props;
    HRESULT hr = dev->OpenPropertyStore(STGM_READ, props.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IMMDevice::OpenPropertyStore", "mode=STGM_READ", ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "IMMDevice::OpenPropertyStore", "mode=STGM_READ", ::wa::log::hrName(hr));
        return HrToResult(hr, "OpenPropertyStore");
    }
    if (!readFormatKey(props.Get(), PKEY_AudioEngine_DeviceFormat, out))
        return Result::Fail(1, "DeviceFormat not present");
    return Result::Ok();
}

Result DeviceEnumerator::oemFormat(const DeviceId& id, AudioFormat& out) {
    ComInitGuard com;
    if (!com.ok()) return HrToResult(com.hr, "DeviceEnumerator: CoInitializeEx");
    ComPtr<IMMDevice> dev;
    if (Result r = openDevice(DataFlow::Render, id, dev); !r) return r;
    ComPtr<IPropertyStore> props;
    HRESULT hr = dev->OpenPropertyStore(STGM_READ, props.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IMMDevice::OpenPropertyStore", "mode=STGM_READ", ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "IMMDevice::OpenPropertyStore", "mode=STGM_READ", ::wa::log::hrName(hr));
        return HrToResult(hr, "OpenPropertyStore");
    }
    if (!readFormatKey(props.Get(), PKEY_AudioEngine_OEMFormat, out))
        return Result::Fail(1, "OemFormat not present");
    return Result::Ok();
}

Result DeviceEnumerator::queryCapabilities(DataFlow flow, const DeviceId& id, DeviceCapabilities& out) {
    ComInitGuard com;
    if (!com.ok()) return HrToResult(com.hr, "DeviceEnumerator: CoInitializeEx");
    out = DeviceCapabilities{};
    ComPtr<IMMDevice> dev;
    if (Result r = openDevice(flow, id, dev); !r) return r;
    ComPtr<IAudioClient> client;
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                     reinterpret_cast<void**>(client.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IMMDevice::Activate", "iid=IAudioClient", ::wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "DeviceEnum", "IMMDevice::Activate", "iid=IAudioClient", ::wa::log::hrName(hr));
        return HrToResult(hr, "Activate");
    }
    WAVEFORMATEX* mix = nullptr;
    HRESULT gmHr = client->GetMixFormat(&mix);
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IAudioClient::GetMixFormat", "", ::wa::log::hrName(gmHr));
    if (SUCCEEDED(gmHr) && mix) {
        out.mixFormat = fromWaveFormat(mix); out.hasMix = true; CoTaskMemFree(mix);
    }
    ComPtr<IPropertyStore> props;
    HRESULT opsHr = dev->OpenPropertyStore(STGM_READ, props.GetAddressOf());
    WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IMMDevice::OpenPropertyStore", "mode=STGM_READ", ::wa::log::hrName(opsHr));
    if (SUCCEEDED(opsHr)) {
        out.hasDevice = readFormatKey(props.Get(), PKEY_AudioEngine_DeviceFormat, out.deviceFormat);
        out.hasOem    = readFormatKey(props.Get(), PKEY_AudioEngine_OEMFormat,    out.oemFormat);
    }
    auto probe = [&](const AudioFormat& f, AUDCLNT_SHAREMODE sm) -> HRESULT {
        WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(f);
        WAVEFORMATEX* closest = nullptr;
        HRESULT h = client->IsFormatSupported(sm, reinterpret_cast<WAVEFORMATEX*>(&wfx),
                        (sm == AUDCLNT_SHAREMODE_EXCLUSIVE) ? nullptr : &closest);
        WA_LOG(wa::log::Level::Debug, "DeviceEnum", "IAudioClient::IsFormatSupported",
               wa::formatAudio(f) + (sm == AUDCLNT_SHAREMODE_EXCLUSIVE ? " excl" : " shared"),
               ::wa::log::hrName(h));
        if (closest) CoTaskMemFree(closest);
        return h;
    };
    // Shared 鐨?S_FALSE(鍙浆鎹絾闈炵簿纭? 涔熺畻"鍙敤"锛汦xclusive 浠呬弗鏍?S_OK銆?
    out.matrix = buildCapabilityMatrix(capabilityCandidates(out),
        [&](const AudioFormat& f) {
            const HRESULT h = probe(f, AUDCLNT_SHAREMODE_SHARED);
            return h == S_OK ? SupportLevel::Exact
                 : h == S_FALSE ? SupportLevel::ClosestMatch
                                : SupportLevel::Unsupported;
        },
        [&](const AudioFormat& f) {
            return probe(f, AUDCLNT_SHAREMODE_EXCLUSIVE) == S_OK
                ? SupportLevel::Exact : SupportLevel::Unsupported;
        });
    // 涓夋潵婧?
    return Result::Ok();
}

} // namespace wa
