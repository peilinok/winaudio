#include "device_notification_client.h"

namespace winaudio {

DeviceNotificationClient::DeviceNotificationClient(HWND hwnd) : hwnd_(hwnd) {}

bool DeviceNotificationClient::Register() {
  if (registered_) {
    return true;
  }
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator_))) ||
      !enumerator_) {
    return false;
  }
  if (FAILED(enumerator_->RegisterEndpointNotificationCallback(this))) {
    enumerator_.Reset();
    return false;
  }
  registered_ = true;
  return true;
}

void DeviceNotificationClient::Unregister() {
  if (registered_ && enumerator_) {
    enumerator_->UnregisterEndpointNotificationCallback(this);
  }
  registered_ = false;
  enumerator_.Reset();
}

ULONG STDMETHODCALLTYPE DeviceNotificationClient::AddRef() {
  return ++ref_count_;
}

ULONG STDMETHODCALLTYPE DeviceNotificationClient::Release() {
  const auto count = --ref_count_;
  if (count == 0) {
    delete this;
  }
  return count;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::QueryInterface(
    REFIID riid, VOID** ppv_interface) {
  if (ppv_interface == nullptr) {
    return E_POINTER;
  }
  *ppv_interface = nullptr;
  if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
    *ppv_interface = static_cast<IMMNotificationClient*>(this);
    AddRef();
    return S_OK;
  }
  return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceStateChanged(
    LPCWSTR pwstrDeviceId, DWORD dwNewState) {
  (void)pwstrDeviceId;
  (void)dwNewState;
  NotifyUi(1, eAll);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceAdded(
    LPCWSTR pwstrDeviceId) {
  (void)pwstrDeviceId;
  NotifyUi(2, eAll);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDeviceRemoved(
    LPCWSTR pwstrDeviceId) {
  (void)pwstrDeviceId;
  NotifyUi(3, eAll);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnDefaultDeviceChanged(
    EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId) {
  (void)role;
  (void)pwstrDefaultDeviceId;
  NotifyUi(4, flow);
  return S_OK;
}

HRESULT STDMETHODCALLTYPE DeviceNotificationClient::OnPropertyValueChanged(
    LPCWSTR pwstrDeviceId, const PROPERTYKEY key) {
  (void)pwstrDeviceId;
  (void)key;
  NotifyUi(5, eAll);
  return S_OK;
}

void DeviceNotificationClient::NotifyUi(UINT reason, EDataFlow flow) {
  if (hwnd_ != nullptr) {
    PostMessageW(hwnd_, WM_APP_DEVICE_CHANGE, static_cast<WPARAM>(reason),
                 static_cast<LPARAM>(flow));
  }
}

}  // namespace winaudio
