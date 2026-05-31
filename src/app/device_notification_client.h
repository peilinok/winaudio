#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <atomic>

namespace winaudio {

constexpr UINT WM_APP_DEVICE_CHANGE = WM_APP + 100;

class DeviceNotificationClient final : public IMMNotificationClient {
 public:
  explicit DeviceNotificationClient(HWND hwnd);

  bool Register();
  void Unregister();

  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppv_interface) override;

  HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR pwstrDeviceId,
                                                 DWORD dwNewState) override;
  HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR pwstrDeviceId) override;
  HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR pwstrDeviceId) override;
  HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                   LPCWSTR pwstrDefaultDeviceId) override;
  HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR pwstrDeviceId,
                                                   const PROPERTYKEY key) override;

 private:
  void NotifyUi(UINT reason, EDataFlow flow);

  std::atomic<ULONG> ref_count_ {1};
  HWND hwnd_ = nullptr;
  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
  bool registered_ = false;
};

}  // namespace winaudio
