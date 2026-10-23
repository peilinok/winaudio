#include "PolicyConfig.h"
#include "ComUtil.h"
#include "Log.h"
#include <mmreg.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>

namespace wa {
namespace {

// {870af99c-171d-4f9e-af0d-e63df40c2bc9} — PolicyConfigClient
static const CLSID kClsidPolicyConfigClient =
    {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

// IID {f8679f50-850a-41cf-9c72-430f290290c8}
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, BOOL, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, BOOL, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&,
                                                       PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, BOOL, const PROPERTYKEY&,
                                                       PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, BOOL) = 0;
};

Result createPolicyConfig(ComPtr<IPolicyConfig>& out) {
    HRESULT hr = CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL,
                                  __uuidof(IPolicyConfig),
                                  reinterpret_cast<void**>(out.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "PolicyConfig", "CoCreateInstance(PolicyConfigClient)", "",
           wa::log::hrName(hr));
    if (FAILED(hr)) return HrToResult(hr, "PolicyConfig: CoCreateInstance");
    return Result::Ok();
}

bool ulValFromPropVariant(const PROPVARIANT& pv, uint32_t& out) {
    if (pv.vt == VT_UI4 || pv.vt == VT_I4 || pv.vt == VT_UINT || pv.vt == VT_INT) {
        out = pv.ulVal;
        return true;
    }
    if (pv.vt == VT_BOOL) {
        // VARIANT_TRUE => disabled (mirrors DeviceEnumerator / EndpointGraphReader).
        out = (pv.boolVal != VARIANT_FALSE) ? 1u : 0u;
        return true;
    }
    return false;
}

std::optional<uint32_t> getDisableSysFx(IPolicyConfig* pc, const DeviceId& id, BOOL bFxStore) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    HRESULT hr = pc->GetPropertyValue(id.c_str(), bFxStore, PKEY_AudioEndpoint_Disable_SysFx, &pv);
    WA_LOG(wa::log::Level::Debug, "PolicyConfig", "GetPropertyValue(Disable_SysFx)",
           std::string(bFxStore ? "fx" : "endpoint") +
               " vt=" + std::to_string(static_cast<int>(pv.vt)),
           wa::log::hrName(hr));
    std::optional<uint32_t> out;
    if (SUCCEEDED(hr)) {
        uint32_t v = 0;
        if (ulValFromPropVariant(pv, v)) out = v;
    }
    PropVariantClear(&pv);
    return out;
}

Result setDisableSysFx(IPolicyConfig* pc, const DeviceId& id, BOOL bFxStore, uint32_t ulVal) {
    PROPVARIANT pv;
    PropVariantInit(&pv);
    pv.vt = VT_UI4;
    pv.ulVal = ulVal;
    HRESULT hr = pc->SetPropertyValue(id.c_str(), bFxStore, PKEY_AudioEndpoint_Disable_SysFx, &pv);
    WA_LOG(wa::log::Level::Info, "PolicyConfig", "SetPropertyValue(Disable_SysFx)",
           std::string(bFxStore ? "fx" : "endpoint") + " ulVal=" + std::to_string(ulVal),
           wa::log::hrName(hr));
    PropVariantClear(&pv);
    if (FAILED(hr)) {
        return HrToResult(hr, bFxStore ? "PolicyConfig: SetPropertyValue(Fx store)"
                                       : "PolicyConfig: SetPropertyValue(endpoint store)");
    }
    return Result::Ok();
}

}  // namespace

std::optional<uint32_t> readDisableSysFxUlVal(const DeviceId& endpointId) {
    if (endpointId.empty()) return std::nullopt;
    ComPtr<IPolicyConfig> pc;
    if (!createPolicyConfig(pc)) return std::nullopt;
    if (auto fx = getDisableSysFx(pc.Get(), endpointId, TRUE)) return fx;
    if (auto ep = getDisableSysFx(pc.Get(), endpointId, FALSE)) return ep;
    return std::nullopt;
}

Result writeDisableSysFxUlVal(const DeviceId& endpointId, uint32_t ulVal) {
    if (endpointId.empty()) {
        return Result::Fail(-1, "PolicyConfig: empty endpoint id");
    }
    ComPtr<IPolicyConfig> pc;
    if (Result r = createPolicyConfig(pc); !r) return r;

    // Fx store first: this is what Sound CPL / Win11 Settings toggle for APOs.
    if (Result r = setDisableSysFx(pc.Get(), endpointId, TRUE, ulVal); !r) return r;
    // Endpoint store second: keeps IMMDevice::OpenPropertyStore readers in sync.
    if (Result r = setDisableSysFx(pc.Get(), endpointId, FALSE, ulVal); !r) {
        WA_LOG(wa::log::Level::Warn, "PolicyConfig", "endpoint store write failed after Fx ok",
               "", r.message);
        return r;
    }
    return Result::Ok();
}

}  // namespace wa