#include "DeviceEnumerator.h"
#include "ComUtil.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include "Log.h"
#include "AudioFormatStr.h"

namespace wa {

namespace {
EDataFlow toEDataFlow(DataFlow f) { return f == DataFlow::Capture ? eCapture : eRender; }

// 读一个 PKEY 的 WAVEFORMATEX blob -> AudioFormat；成功返回 true。
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
    // Shared 的 S_FALSE(可转换但非精确) 也算"可用"；Exclusive 仅严格 S_OK。
    out.matrix = buildCapabilityMatrix(allFormatCandidates(),
        [&](const AudioFormat& f){ HRESULT h = probe(f, AUDCLNT_SHAREMODE_SHARED); return h == S_OK || h == S_FALSE; },
        [&](const AudioFormat& f){ return probe(f, AUDCLNT_SHAREMODE_EXCLUSIVE) == S_OK; });
    // 三来源
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
    return Result::Ok();
}

} // namespace wa
