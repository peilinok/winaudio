#include "DeviceEnumerator.h"
#include "ComUtil.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

namespace wa {

namespace {
EDataFlow toEDataFlow(DataFlow f) { return f == DataFlow::Capture ? eCapture : eRender; }

Result readInfo(IMMDevice* dev, DataFlow flow, bool isDefault, DeviceInfo& info) {
    LPWSTR idStr = nullptr;
    HRESULT hr = dev->GetId(&idStr);
    if (FAILED(hr)) return HrToResult(hr, "IMMDevice::GetId");
    info.id = idStr;
    CoTaskMemFree(idStr);
    info.flow = flow;
    info.isDefault = isDefault;

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT name; PropVariantInit(&name);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) &&
            name.vt == VT_LPWSTR)
            info.name = name.pwszVal;
        PropVariantClear(&name);
    }

    ComPtr<IAudioClient> client;
    if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(client.GetAddressOf())))) {
        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
            info.mixFormat = fromWaveFormat(mix);
            CoTaskMemFree(mix);
        }
    }
    return Result::Ok();
}
} // namespace

Result DeviceEnumerator::enumerate(DataFlow flow, std::vector<DeviceInfo>& out) {
    out.clear();
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "CoCreateInstance(MMDeviceEnumerator)");

    DeviceId defaultId;
    {
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(e->GetDefaultAudioEndpoint(toEDataFlow(flow), eConsole, &def))) {
            LPWSTR s = nullptr;
            if (SUCCEEDED(def->GetId(&s))) { defaultId = s; CoTaskMemFree(s); }
        }
    }

    ComPtr<IMMDeviceCollection> coll;
    hr = e->EnumAudioEndpoints(toEDataFlow(flow), DEVICE_STATE_ACTIVE, &coll);
    if (FAILED(hr)) return HrToResult(hr, "EnumAudioEndpoints");

    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;
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
    if (FAILED(hr)) return HrToResult(hr, "CoCreateInstance(MMDeviceEnumerator)");
    ComPtr<IMMDevice> dev;
    hr = e->GetDefaultAudioEndpoint(toEDataFlow(flow), eConsole, &dev);
    if (FAILED(hr)) return HrToResult(hr, "GetDefaultAudioEndpoint");
    return readInfo(dev.Get(), flow, true, out);
}

Result DeviceEnumerator::mixFormat(const DeviceId& id, AudioFormat& out) {
    ComPtr<IMMDeviceEnumerator> e;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(e.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "CoCreateInstance(MMDeviceEnumerator)");
    ComPtr<IMMDevice> dev;
    if (id.empty())
        hr = e->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    else
        hr = e->GetDevice(id.c_str(), &dev);
    if (FAILED(hr)) return HrToResult(hr, "GetDevice");

    ComPtr<IAudioClient> client;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                       reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return HrToResult(hr, "IMMDevice::Activate");
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr)) return HrToResult(hr, "GetMixFormat");
    out = fromWaveFormat(mix);
    CoTaskMemFree(mix);
    return Result::Ok();
}

} // namespace wa
