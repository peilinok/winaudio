#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "real_backends.h"

#include <avrt.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

#include "audio/com_support.h"

namespace winaudio {

using Microsoft::WRL::ComPtr;

namespace {

bool IsWindowsBuildAtLeast(DWORD build_number) {
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) {
    return false;
  }
  using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
  const auto rtl_get_version =
      reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
  if (rtl_get_version == nullptr) {
    return false;
  }
  RTL_OSVERSIONINFOW version_info {};
  version_info.dwOSVersionInfoSize = sizeof(version_info);
  if (rtl_get_version(&version_info) != 0) {
    return false;
  }
  return version_info.dwMajorVersion > 10 ||
         (version_info.dwMajorVersion == 10 &&
          version_info.dwBuildNumber >= build_number);
}

std::wstring MmResultToString(MMRESULT result) {
  switch (result) {
    case MMSYSERR_NOERROR:
      return L"MMSYSERR_NOERROR";
    case MMSYSERR_ALLOCATED:
      return L"MMSYSERR_ALLOCATED";
    case MMSYSERR_BADDEVICEID:
      return L"MMSYSERR_BADDEVICEID";
    case MMSYSERR_NODRIVER:
      return L"MMSYSERR_NODRIVER";
    case MMSYSERR_NOMEM:
      return L"MMSYSERR_NOMEM";
    case MMSYSERR_INVALPARAM:
      return L"MMSYSERR_INVALPARAM";
    case WAVERR_BADFORMAT:
      return L"WAVERR_BADFORMAT";
    default:
      return L"MMRESULT(" + std::to_wstring(result) + L")";
  }
}

std::wstring WasapiResultToString(HRESULT hr) {
  switch (hr) {
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
      return L"AUDCLNT_E_UNSUPPORTED_FORMAT";
    case AUDCLNT_E_DEVICE_INVALIDATED:
      return L"AUDCLNT_E_DEVICE_INVALIDATED";
    case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:
      return L"AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
    case AUDCLNT_E_EVENTHANDLE_NOT_SET:
      return L"AUDCLNT_E_EVENTHANDLE_NOT_SET";
    case AUDCLNT_E_WRONG_ENDPOINT_TYPE:
      return L"AUDCLNT_E_WRONG_ENDPOINT_TYPE";
    case AUDCLNT_E_NOT_INITIALIZED:
      return L"AUDCLNT_E_NOT_INITIALIZED";
    case AUDCLNT_E_NOT_STOPPED:
      return L"AUDCLNT_E_NOT_STOPPED";
    default:
      return HResultToString(hr);
  }
}

class AudioInterfaceActivateHandler final
    : public IActivateAudioInterfaceCompletionHandler {
 public:
  AudioInterfaceActivateHandler() = default;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid,
                                           void** object) override {
    if (object == nullptr) {
      return E_POINTER;
    }
    *object = nullptr;
    if (riid == __uuidof(IUnknown) ||
        riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
      *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }

  ULONG STDMETHODCALLTYPE AddRef() override {
    return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
  }

  ULONG STDMETHODCALLTYPE Release() override {
    const auto count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
    if (count == 0) {
      delete this;
    }
    return count;
  }

  HRESULT STDMETHODCALLTYPE ActivateCompleted(
      IActivateAudioInterfaceAsyncOperation* activateOperation) override {
    HRESULT activate_result = E_FAIL;
    IUnknown* activated_interface = nullptr;
    if (activateOperation != nullptr) {
      activateOperation->GetActivateResult(&activate_result, &activated_interface);
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      activate_result_ = activate_result;
      activated_interface_.Attach(activated_interface);
      completed_ = true;
    }
    cv_.notify_all();
    return S_OK;
  }

  HRESULT Wait(DWORD timeout_ms, ComPtr<IAudioClient>* audio_client) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                      [&]() { return completed_; })) {
      return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    }

    if (audio_client != nullptr) {
      audio_client->Reset();
      if (activated_interface_) {
        activated_interface_.As(audio_client);
      }
    }
    return activate_result_;
  }

 private:
  ~AudioInterfaceActivateHandler() = default;

  LONG ref_count_ = 1;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool completed_ = false;
  HRESULT activate_result_ = E_FAIL;
  ComPtr<IUnknown> activated_interface_;
};

void AppendSharedEnginePeriodDetail(IAudioClient* client, std::wstring* details) {
  if (client == nullptr || details == nullptr) {
    return;
  }

  Microsoft::WRL::ComPtr<IAudioClient3> client3;
  if (FAILED(client->QueryInterface(__uuidof(IAudioClient3),
                                    reinterpret_cast<void**>(
                                        client3.GetAddressOf()))) ||
      !client3) {
    return;
  }

  WAVEFORMATEX* current_format = nullptr;
  UINT32 current_period_frames = 0;
  if (SUCCEEDED(client3->GetCurrentSharedModeEnginePeriod(
          &current_format, &current_period_frames))) {
    *details +=
        L"; sharedEnginePeriodFrames=" + std::to_wstring(current_period_frames);
    if (current_format != nullptr) {
      CoTaskMemFree(current_format);
    }
  }
}

constexpr wchar_t kDefaultWaveId[] = L"default";

bool IsWaveLoopbackName(const std::wstring& name) {
  std::wstring lower = name;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
  return lower.find(L"stereo mix") != std::wstring::npos ||
         lower.find(L"what u hear") != std::wstring::npos ||
         lower.find(L"what you hear") != std::wstring::npos ||
         lower.find(L"wave out mix") != std::wstring::npos;
}

std::wstring GetDeviceFriendlyName(IMMDevice* device) {
  ComPtr<IPropertyStore> store;
  if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) {
    return L"Unknown Device";
  }

  PROPVARIANT value {};
  PropVariantInit(&value);
  std::wstring result = L"Unknown Device";
  if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
      value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
    result = value.pwszVal;
  }
  PropVariantClear(&value);
  return result;
}

std::wstring GetDeviceId(IMMDevice* device) {
  LPWSTR id = nullptr;
  if (FAILED(device->GetId(&id)) || id == nullptr) {
    return {};
  }
  std::wstring value(id);
  CoTaskMemFree(id);
  return value;
}

std::vector<AudioDeviceDescriptor> EnumerateWasapiDevices(EDataFlow flow) {
  std::vector<AudioDeviceDescriptor> devices;

  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator)))) {
    return devices;
  }

  ComPtr<IMMDevice> default_device;
  std::wstring default_id;
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eConsole, &default_device))) {
    default_id = GetDeviceId(default_device.Get());
  }

  ComPtr<IMMDeviceCollection> collection;
  if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection))) {
    return devices;
  }

  UINT count = 0;
  collection->GetCount(&count);
  for (UINT index = 0; index < count; ++index) {
    ComPtr<IMMDevice> device;
    if (FAILED(collection->Item(index, &device))) {
      continue;
    }
    AudioDeviceDescriptor descriptor;
    descriptor.id = GetDeviceId(device.Get());
    descriptor.friendly_name = GetDeviceFriendlyName(device.Get());
    descriptor.direction =
        flow == eCapture ? AudioDirection::Capture : AudioDirection::Render;
    descriptor.is_default = descriptor.id == default_id;
    descriptor.supports_loopback = flow == eRender;
    descriptor.capability_flags = kDeviceCapabilitySharedMode |
                                  kDeviceCapabilityExclusiveMode |
                                  kDeviceCapabilityEventDriven |
                                  kDeviceCapabilityTimerDriven;
    devices.push_back(std::move(descriptor));
  }

  return devices;
}

ComPtr<IMMDevice> ResolveWasapiDevice(EDataFlow flow,
                                      const std::wstring& device_id) {
  ComPtr<IMMDeviceEnumerator> enumerator;
  if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                              IID_PPV_ARGS(&enumerator)))) {
    return nullptr;
  }

  ComPtr<IMMDevice> device;
  if (device_id.empty()) {
    enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
  } else {
    enumerator->GetDevice(device_id.c_str(), &device);
  }
  return device;
}

AudioFormatSpec ChooseWaveCompatibleFormat(const AudioFormatSpec& requested) {
  AudioFormatSpec format = requested;
  format.sample_type = AudioSampleType::PcmInt16;
  format.bits_per_sample = 16;
  if (format.sample_rate == 0) {
    format.sample_rate = 48000;
  }
  if (format.channels == 0) {
    format.channels = 2;
  }
  format.normalize();
  return format;
}

MMRESULT QueryWaveInFormat(UINT device_id, const WAVEFORMATEX& format) {
  return waveInOpen(nullptr, device_id, const_cast<WAVEFORMATEX*>(&format), 0, 0,
                    WAVE_FORMAT_QUERY);
}

MMRESULT QueryWaveOutFormat(UINT device_id, const WAVEFORMATEX& format) {
  return waveOutOpen(nullptr, device_id, const_cast<WAVEFORMATEX*>(&format), 0, 0,
                     WAVE_FORMAT_QUERY);
}

std::optional<DWORD> ResolveProcessLoopbackTargetProcessId(
    ApplicationLoopbackTargetKind target_kind,
    const std::wstring& target) {
  if (target.empty()) {
    return std::nullopt;
  }

  if (target_kind == ApplicationLoopbackTargetKind::ProcessId) {
    try {
      const auto parsed = static_cast<DWORD>(std::stoul(target));
      if (parsed != 0) {
        return parsed;
      }
    } catch (...) {
    }
    return std::nullopt;
  }

  const HANDLE snapshot =
      CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return std::nullopt;
  }

  PROCESSENTRY32W entry {};
  entry.dwSize = sizeof(entry);
  std::vector<DWORD> matches;
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (_wcsicmp(entry.szExeFile, target.c_str()) == 0) {
        matches.push_back(entry.th32ProcessID);
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  if (matches.empty()) {
    return std::nullopt;
  }

  ComPtr<IMMDeviceEnumerator> enumerator;
  if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 IID_PPV_ARGS(&enumerator))) &&
      enumerator) {
    ComPtr<IMMDeviceCollection> render_collection;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(
            eRender, DEVICE_STATE_ACTIVE, render_collection.GetAddressOf())) &&
        render_collection) {
      UINT render_count = 0;
      if (SUCCEEDED(render_collection->GetCount(&render_count))) {
        for (UINT render_index = 0; render_index < render_count; ++render_index) {
          ComPtr<IMMDevice> render_device;
          if (FAILED(render_collection->Item(render_index, render_device.GetAddressOf())) ||
              !render_device) {
            continue;
          }
          ComPtr<IAudioSessionManager2> session_manager;
          if (FAILED(render_device->Activate(__uuidof(IAudioSessionManager2),
                                             CLSCTX_ALL, nullptr,
                                             reinterpret_cast<void**>(
                                                 session_manager.GetAddressOf()))) ||
              !session_manager) {
            continue;
          }
          ComPtr<IAudioSessionEnumerator> session_enumerator;
          if (FAILED(session_manager->GetSessionEnumerator(
                  session_enumerator.GetAddressOf())) ||
              !session_enumerator) {
            continue;
          }
          int session_count = 0;
          if (FAILED(session_enumerator->GetCount(&session_count))) {
            continue;
          }
          for (int index = 0; index < session_count; ++index) {
            ComPtr<IAudioSessionControl> session_control;
            if (FAILED(session_enumerator->GetSession(index,
                                                      session_control.GetAddressOf())) ||
                !session_control) {
              continue;
            }
            AudioSessionState session_state = AudioSessionStateInactive;
            if (FAILED(session_control->GetState(&session_state)) ||
                session_state != AudioSessionStateActive) {
              continue;
            }
            ComPtr<IAudioSessionControl2> session_control2;
            if (FAILED(session_control.As(&session_control2)) || !session_control2) {
              continue;
            }
            DWORD process_id = 0;
            if (FAILED(session_control2->GetProcessId(&process_id)) ||
                process_id == 0) {
              continue;
            }
            if (std::find(matches.begin(), matches.end(), process_id) !=
                matches.end()) {
              return process_id;
            }
          }
        }
      }
    }
  }

  return std::nullopt;
}

}  // namespace

std::wstring WasapiCaptureAdapter::name() const {
  return L"WASAPI Capture";
}

AudioBackendType WasapiCaptureAdapter::backend_type() const {
  return AudioBackendType::Wasapi;
}

bool WasapiCaptureAdapter::SupportsSource(AudioSourceMode source_mode) const {
  return source_mode == AudioSourceMode::MicrophoneCapture ||
         source_mode == AudioSourceMode::SystemLoopback ||
         source_mode == AudioSourceMode::ApplicationProcessLoopback ||
         source_mode == AudioSourceMode::ApplicationLoopback;
}

std::vector<AudioDeviceDescriptor> WasapiCaptureAdapter::EnumerateDevices(
    AudioSourceMode source_mode) {
  if (source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      source_mode == AudioSourceMode::ApplicationLoopback) {
    AudioDeviceDescriptor descriptor;
    descriptor.id = L"app-loopback";
    descriptor.friendly_name =
        source_mode == AudioSourceMode::ApplicationProcessLoopback
            ? L"Application Process Loopback Target"
            : L"Application Loopback Target";
    descriptor.direction = AudioDirection::Capture;
    descriptor.is_default = true;
    descriptor.supports_loopback = true;
    descriptor.capability_flags = kDeviceCapabilitySharedMode |
                                  kDeviceCapabilityEventDriven;
    return {descriptor};
  }
  return EnumerateWasapiDevices(source_mode == AudioSourceMode::SystemLoopback
                                    ? eRender
                                    : eCapture);
}

std::optional<AudioFormatSpec> WasapiCaptureAdapter::GetPreferredFormat(
    const CaptureConfig& config) {
  if (config.source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      config.source_mode == AudioSourceMode::ApplicationLoopback) {
    if (!IsProcessLoopbackSupportedOnCurrentWindows()) {
      last_error_ = L"app-loopback-unsupported-os";
      return std::nullopt;
    }
    Microsoft::WRL::ComPtr<IAudioClient> client;
    if (!ActivateProcessLoopbackClient(config, &client) || !client) {
      return std::nullopt;
    }
    auto requested = MakeWaveFormatExtensible(config.format);
    WAVEFORMATEX* mix_format = nullptr;
    client->GetMixFormat(&mix_format);

    AudioFormatSpec resolved = config.format;
    resolved.normalize();

    if (config.wasapi_share_mode == WasapiShareMode::Shared) {
      WAVEFORMATEX* closest_match = nullptr;
      const auto hr = client->IsFormatSupported(
          AUDCLNT_SHAREMODE_SHARED,
          reinterpret_cast<WAVEFORMATEX*>(&requested), &closest_match);
      if (hr == S_OK) {
        resolved = AudioFormatFromWaveFormat(
            reinterpret_cast<const WAVEFORMATEX&>(requested));
      } else if (closest_match != nullptr) {
        resolved = AudioFormatFromWaveFormat(*closest_match);
        CoTaskMemFree(closest_match);
      } else if (mix_format != nullptr) {
        resolved = AudioFormatFromWaveFormat(*mix_format);
      }
    } else {
      if (mix_format != nullptr) {
        resolved = AudioFormatFromWaveFormat(*mix_format);
      }
    }

    if (mix_format != nullptr) {
      CoTaskMemFree(mix_format);
    }
    return resolved;
  }
  auto device = ResolveWasapiDevice(
      config.source_mode == AudioSourceMode::SystemLoopback ? eRender : eCapture,
      config.device_id);
  if (!device) {
    return std::nullopt;
  }
  return ResolveFormat(config, device.Get());
}

bool WasapiCaptureAdapter::Start(const CaptureConfig& config,
                                 const AudioFormatSpec& runtime_format,
                                 ISessionEventSink* sink) {
  (void)sink;
  Stop();
  last_error_.clear();
  source_mode_ = config.source_mode;
  runtime_format_ = runtime_format;
  runtime_format_.normalize();
  runtime_mode_.clear();

  if (config.source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      config.source_mode == AudioSourceMode::ApplicationLoopback) {
    if (config.application_loopback_target_value.empty()) {
      last_error_ = config.source_mode ==
                            AudioSourceMode::ApplicationProcessLoopback
                        ? L"app-loopback-process-id-required"
                        : L"app-loopback-application-required";
      return false;
    }
    if (!IsProcessLoopbackSupportedOnCurrentWindows()) {
      last_error_ = L"app-loopback-unsupported-os";
      return false;
    }
    if (!ActivateProcessLoopbackClient(config, &audio_client_) || !audio_client_) {
      return false;
    }
  } else {
    ComPtr<IMMDevice> device;
    if (!ActivateForConfig(config, &device) || !device) {
      last_error_ = L"resolve-device";
      return false;
    }

    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(
                                    audio_client_.GetAddressOf())))) {
      last_error_ = L"activate-iaudioclient";
      return false;
    }
  }

  auto wave_format = MakeWaveFormatExtensible(runtime_format_);
  DWORD stream_flags = 0;
  if (config.source_mode == AudioSourceMode::SystemLoopback) {
    stream_flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
  }
  if (config.wasapi_drive_mode == WasapiDriveMode::EventDriven) {
    stream_flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }

  const auto share_mode = config.wasapi_share_mode == WasapiShareMode::Exclusive
                              ? AUDCLNT_SHAREMODE_EXCLUSIVE
                              : AUDCLNT_SHAREMODE_SHARED;
  const REFERENCE_TIME buffer_duration =
      static_cast<REFERENCE_TIME>(config.buffer_duration_ms) * 10000;
  HRESULT hr = audio_client_->Initialize(
      share_mode, stream_flags, buffer_duration,
      share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE ? buffer_duration : 0,
      reinterpret_cast<WAVEFORMATEX*>(&wave_format), nullptr);
  if (FAILED(hr) &&
      (stream_flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0) {
    stream_flags &= ~AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = audio_client_->Initialize(
        share_mode, stream_flags, buffer_duration,
        share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE ? buffer_duration : 0,
        reinterpret_cast<WAVEFORMATEX*>(&wave_format), nullptr);
  }
  if (FAILED(hr)) {
    last_error_ = L"Initialize: " + WasapiResultToString(hr);
    audio_client_.Reset();
    return false;
  }

  runtime_mode_ = L"WASAPI " +
                  std::wstring(share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE
                                   ? L"Exclusive"
                                   : L"Shared") +
                  L" / " +
                  std::wstring((stream_flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0
                                   ? L"Event"
                                   : L"Timer");

  if ((stream_flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0) {
    event_handle_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle_ != nullptr) {
      audio_client_->SetEventHandle(event_handle_);
    }
  }

  if (FAILED(audio_client_->GetService(__uuidof(IAudioCaptureClient),
                                       reinterpret_cast<void**>(
                                           capture_client_.GetAddressOf())))) {
    last_error_ = L"GetService(IAudioCaptureClient)";
    Stop();
    return false;
  }

  const auto start_hr = audio_client_->Start();
  if (FAILED(start_hr)) {
    last_error_ = L"Start: " + WasapiResultToString(start_hr);
    Stop();
    return false;
  }

  REFERENCE_TIME default_period = 0;
  REFERENCE_TIME minimum_period = 0;
  REFERENCE_TIME stream_latency = 0;
  audio_client_->GetDevicePeriod(&default_period, &minimum_period);
  audio_client_->GetStreamLatency(&stream_latency);
  runtime_details_ = L"devicePeriod100ns=" + std::to_wstring(default_period) +
                     L"; minPeriod100ns=" + std::to_wstring(minimum_period) +
                     L"; streamLatency100ns=" + std::to_wstring(stream_latency);
  if (share_mode == AUDCLNT_SHAREMODE_SHARED) {
    AppendSharedEnginePeriodDetail(audio_client_.Get(), &runtime_details_);
  }

  frame_cursor_ = 0;
  return true;
}

void WasapiCaptureAdapter::Stop() {
  if (audio_client_) {
    audio_client_->Stop();
  }
  capture_client_.Reset();
  audio_client_.Reset();
  if (event_handle_ != nullptr) {
    CloseHandle(event_handle_);
    event_handle_ = nullptr;
  }
}

std::optional<AudioFrameChunk> WasapiCaptureAdapter::ReadChunk() {
  if (!capture_client_) {
    return std::nullopt;
  }

  UINT32 packet_frames = 0;
  if (FAILED(capture_client_->GetNextPacketSize(&packet_frames)) ||
      packet_frames == 0) {
    return std::nullopt;
  }

  AudioFrameChunk chunk;
  chunk.format = runtime_format_;
  chunk.frame_index = frame_cursor_;

  uint64_t first_qpc = 0;
  while (packet_frames > 0) {
    BYTE* data = nullptr;
    UINT32 frames = 0;
    DWORD flags = 0;
    UINT64 device_position = 0;
    UINT64 qpc_position = 0;
    if (FAILED(capture_client_->GetBuffer(&data, &frames, &flags, &device_position,
                                          &qpc_position))) {
      break;
    }
    if (first_qpc == 0) {
      first_qpc = static_cast<uint64_t>(qpc_position);
    }

    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr) {
      chunk.interleaved_samples.resize(
          chunk.interleaved_samples.size() +
              static_cast<size_t>(frames) * runtime_format_.channels,
          0.0f);
    } else {
      const auto samples = ConvertPcmToFloat(
          data, frames, reinterpret_cast<const WAVEFORMATEX&>(
                           MakeWaveFormatExtensible(runtime_format_)));
      chunk.interleaved_samples.insert(chunk.interleaved_samples.end(),
                                       samples.begin(), samples.end());
    }

    capture_client_->ReleaseBuffer(frames);
    frame_cursor_ += frames;
    if (FAILED(capture_client_->GetNextPacketSize(&packet_frames))) {
      break;
    }
  }

  if (chunk.interleaved_samples.empty()) {
    return std::nullopt;
  }
  chunk.first_sample_qpc = static_cast<int64_t>(first_qpc);
  return chunk;
}

std::wstring WasapiCaptureAdapter::last_error() const {
  return last_error_;
}

std::wstring WasapiCaptureAdapter::runtime_mode() const {
  return runtime_mode_;
}

std::wstring WasapiCaptureAdapter::runtime_details() const {
  return runtime_details_;
}

bool WasapiCaptureAdapter::IsProcessLoopbackSupportedOnCurrentWindows() {
  return IsWindowsBuildAtLeast(20348);
}

std::optional<AudioFormatSpec> WasapiCaptureAdapter::ResolveFormat(
    const CaptureConfig& config, IMMDevice* device) {
  ComPtr<IAudioClient> client;
  if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client.GetAddressOf())))) {
    return std::nullopt;
  }

  auto requested = MakeWaveFormatExtensible(config.format);
  WAVEFORMATEX* mix_format = nullptr;
  client->GetMixFormat(&mix_format);

  AudioFormatSpec resolved = config.format;
  resolved.normalize();

  if (config.wasapi_share_mode == WasapiShareMode::Shared) {
    WAVEFORMATEX* closest_match = nullptr;
    const auto hr = client->IsFormatSupported(
        AUDCLNT_SHAREMODE_SHARED, reinterpret_cast<WAVEFORMATEX*>(&requested),
        &closest_match);
    if (hr == S_OK) {
      resolved = AudioFormatFromWaveFormat(
          reinterpret_cast<const WAVEFORMATEX&>(requested));
    } else if (closest_match != nullptr) {
      resolved = AudioFormatFromWaveFormat(*closest_match);
      CoTaskMemFree(closest_match);
    } else if (mix_format != nullptr) {
      resolved = AudioFormatFromWaveFormat(*mix_format);
    }
  } else {
    const auto hr = client->IsFormatSupported(
        AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&requested),
        nullptr);
    if (FAILED(hr)) {
      if (mix_format != nullptr) {
        CoTaskMemFree(mix_format);
      }
      return std::nullopt;
    }
    resolved = AudioFormatFromWaveFormat(
        reinterpret_cast<const WAVEFORMATEX&>(requested));
  }

  if (mix_format != nullptr) {
    CoTaskMemFree(mix_format);
  }
  return resolved;
}

bool WasapiCaptureAdapter::ActivateForConfig(const CaptureConfig& config,
                                             ComPtr<IMMDevice>* device) {
  if (config.source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      config.source_mode == AudioSourceMode::ApplicationLoopback) {
    device->Reset();
    return false;
  }
  *device = ResolveWasapiDevice(
      config.source_mode == AudioSourceMode::SystemLoopback ? eRender : eCapture,
      config.device_id);
  return device->Get() != nullptr;
}

bool WasapiCaptureAdapter::ActivateProcessLoopbackClient(
    const CaptureConfig& config, ComPtr<IAudioClient>* client) {
  if (client == nullptr) {
    last_error_ = L"app-loopback-invalid-client";
    return false;
  }
  client->Reset();

  const auto process_id =
      ResolveProcessLoopbackTargetProcessId(
          config.application_loopback_target_kind,
          config.application_loopback_target_value);
  if (!process_id.has_value() || *process_id == 0) {
    last_error_ =
        config.application_loopback_target_kind ==
                ApplicationLoopbackTargetKind::ApplicationName
            ? L"app-loopback-no-active-audio-session"
            : L"app-loopback-invalid-target";
    return false;
  }

  AUDIOCLIENT_ACTIVATION_PARAMS activation_params {};
  activation_params.ActivationType =
      AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
  activation_params.ProcessLoopbackParams.TargetProcessId = *process_id;
  activation_params.ProcessLoopbackParams.ProcessLoopbackMode =
      PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;

  PROPVARIANT activation_prop {};
  PropVariantInit(&activation_prop);
  const auto blob_size = static_cast<ULONG>(sizeof(activation_params));
  auto* blob_bytes = static_cast<BYTE*>(CoTaskMemAlloc(blob_size));
  if (blob_bytes == nullptr) {
    last_error_ = L"app-loopback-oom";
    return false;
  }
  memcpy(blob_bytes, &activation_params, blob_size);
  activation_prop.vt = VT_BLOB;
  activation_prop.blob.cbSize = blob_size;
  activation_prop.blob.pBlobData = blob_bytes;

  auto* handler = new AudioInterfaceActivateHandler();
  ComPtr<IActivateAudioInterfaceAsyncOperation> async_operation;
  const auto activate_hr = ActivateAudioInterfaceAsync(
      VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
      &activation_prop, handler, async_operation.GetAddressOf());
  if (FAILED(activate_hr)) {
    handler->Release();
    PropVariantClear(&activation_prop);
    last_error_ = L"ActivateAudioInterfaceAsync: " +
                  WasapiResultToString(activate_hr);
    return false;
  }

  const auto wait_hr = handler->Wait(5000, client);
  handler->Release();
  PropVariantClear(&activation_prop);
  if (FAILED(wait_hr) || !client->Get()) {
    last_error_ = L"app-loopback-activate: " + WasapiResultToString(wait_hr);
    return false;
  }
  return true;
}

std::wstring WasapiRenderAdapter::name() const {
  return L"WASAPI Render";
}

AudioBackendType WasapiRenderAdapter::backend_type() const {
  return AudioBackendType::Wasapi;
}

std::vector<AudioDeviceDescriptor> WasapiRenderAdapter::EnumerateDevices() {
  return EnumerateWasapiDevices(eRender);
}

std::optional<AudioFormatSpec> WasapiRenderAdapter::GetPreferredFormat(
    const RenderConfig& config) {
  auto device = ResolveWasapiDevice(eRender, config.device_id);
  if (!device) {
    return std::nullopt;
  }
  return ResolveFormat(config, device.Get());
}

bool WasapiRenderAdapter::Start(const RenderConfig& config,
                                const AudioFormatSpec& runtime_format,
                                ISessionEventSink* sink) {
  (void)sink;
  Stop();
  last_error_.clear();
  runtime_format_ = runtime_format;
  runtime_format_.normalize();
  runtime_wave_format_ = MakeWaveFormatExtensible(runtime_format_);
  runtime_mode_.clear();

  ComPtr<IMMDevice> device;
  if (!ActivateForConfig(config, &device) || !device) {
    last_error_ = L"resolve-device";
    return false;
  }

  if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(audio_client_.GetAddressOf())))) {
    last_error_ = L"activate-iaudioclient";
    return false;
  }

  DWORD stream_flags = 0;
  if (config.wasapi_drive_mode == WasapiDriveMode::EventDriven) {
    stream_flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
  }
  const auto share_mode = config.wasapi_share_mode == WasapiShareMode::Exclusive
                              ? AUDCLNT_SHAREMODE_EXCLUSIVE
                              : AUDCLNT_SHAREMODE_SHARED;
  const REFERENCE_TIME buffer_duration =
      static_cast<REFERENCE_TIME>(config.buffer_duration_ms) * 10000;
  HRESULT hr = audio_client_->Initialize(
      share_mode, stream_flags, buffer_duration,
      share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE ? buffer_duration : 0,
      reinterpret_cast<WAVEFORMATEX*>(&runtime_wave_format_), nullptr);
  if (FAILED(hr) &&
      (stream_flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0) {
    stream_flags &= ~AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = audio_client_->Initialize(
        share_mode, stream_flags, buffer_duration,
        share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE ? buffer_duration : 0,
        reinterpret_cast<WAVEFORMATEX*>(&runtime_wave_format_), nullptr);
  }
  if (FAILED(hr)) {
    last_error_ = L"Initialize: " + WasapiResultToString(hr);
    audio_client_.Reset();
    return false;
  }

  runtime_mode_ = L"WASAPI " +
                  std::wstring(share_mode == AUDCLNT_SHAREMODE_EXCLUSIVE
                                   ? L"Exclusive"
                                   : L"Shared") +
                  L" / " +
                  std::wstring((stream_flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0
                                   ? L"Event"
                                   : L"Timer");

  if ((stream_flags & AUDCLNT_STREAMFLAGS_EVENTCALLBACK) != 0) {
    event_handle_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_handle_ != nullptr) {
      audio_client_->SetEventHandle(event_handle_);
    }
  }

  if (FAILED(audio_client_->GetBufferSize(&buffer_frames_)) ||
      FAILED(audio_client_->GetService(__uuidof(IAudioRenderClient),
                                       reinterpret_cast<void**>(
                                           render_client_.GetAddressOf())))) {
    last_error_ = L"GetBufferSize/GetService(IAudioRenderClient)";
    Stop();
    return false;
  }

  BYTE* buffer = nullptr;
  if (SUCCEEDED(render_client_->GetBuffer(buffer_frames_, &buffer))) {
    render_client_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
  }

  const auto start_hr = audio_client_->Start();
  if (FAILED(start_hr)) {
    last_error_ = L"Start: " + WasapiResultToString(start_hr);
    Stop();
    return false;
  }

  REFERENCE_TIME default_period = 0;
  REFERENCE_TIME minimum_period = 0;
  REFERENCE_TIME stream_latency = 0;
  audio_client_->GetDevicePeriod(&default_period, &minimum_period);
  audio_client_->GetStreamLatency(&stream_latency);
  runtime_details_ = L"bufferFrames=" + std::to_wstring(buffer_frames_) +
                     L"; devicePeriod100ns=" + std::to_wstring(default_period) +
                     L"; minPeriod100ns=" + std::to_wstring(minimum_period) +
                     L"; streamLatency100ns=" + std::to_wstring(stream_latency);
  if (share_mode == AUDCLNT_SHAREMODE_SHARED) {
    AppendSharedEnginePeriodDetail(audio_client_.Get(), &runtime_details_);
  }

  return true;
}

void WasapiRenderAdapter::Stop() {
  if (audio_client_) {
    audio_client_->Stop();
  }
  render_client_.Reset();
  audio_client_.Reset();
  buffer_frames_ = 0;
  if (event_handle_ != nullptr) {
    CloseHandle(event_handle_);
    event_handle_ = nullptr;
  }
}

bool WasapiRenderAdapter::WriteChunk(const AudioFrameChunk& chunk) {
  if (!render_client_ || !audio_client_) {
    return false;
  }

  UINT32 padding = 0;
  if (FAILED(audio_client_->GetCurrentPadding(&padding))) {
    last_error_ = L"GetCurrentPadding";
    return false;
  }
  const UINT32 available = buffer_frames_ > padding ? buffer_frames_ - padding : 0;
  if (available == 0) {
    return true;
  }

  const UINT32 frames_to_write = std::min<UINT32>(available, chunk.frame_count());
  if (frames_to_write == 0) {
    return true;
  }

  BYTE* buffer = nullptr;
  if (FAILED(render_client_->GetBuffer(frames_to_write, &buffer))) {
    last_error_ = L"IAudioRenderClient::GetBuffer";
    return false;
  }

  std::vector<BYTE> pcm_bytes;
  ConvertFloatToPcm(chunk.interleaved_samples.data(), frames_to_write,
                    reinterpret_cast<const WAVEFORMATEX&>(runtime_wave_format_),
                    &pcm_bytes);
  std::memcpy(buffer, pcm_bytes.data(), pcm_bytes.size());
  const auto hr = render_client_->ReleaseBuffer(frames_to_write, 0);
  if (FAILED(hr)) {
    last_error_ = L"ReleaseBuffer: " + WasapiResultToString(hr);
    return false;
  }
  return true;
}

std::wstring WasapiRenderAdapter::last_error() const {
  return last_error_;
}

std::wstring WasapiRenderAdapter::runtime_mode() const {
  return runtime_mode_;
}

std::wstring WasapiRenderAdapter::runtime_details() const {
  return runtime_details_;
}

std::optional<AudioFormatSpec> WasapiRenderAdapter::ResolveFormat(
    const RenderConfig& config, IMMDevice* device) {
  ComPtr<IAudioClient> client;
  if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client.GetAddressOf())))) {
    return std::nullopt;
  }

  auto requested = MakeWaveFormatExtensible(config.format);
  WAVEFORMATEX* mix_format = nullptr;
  client->GetMixFormat(&mix_format);

  AudioFormatSpec resolved = config.format;
  resolved.normalize();

  if (config.wasapi_share_mode == WasapiShareMode::Shared) {
    WAVEFORMATEX* closest_match = nullptr;
    const auto hr = client->IsFormatSupported(
        AUDCLNT_SHAREMODE_SHARED, reinterpret_cast<WAVEFORMATEX*>(&requested),
        &closest_match);
    if (hr == S_OK) {
      resolved = AudioFormatFromWaveFormat(
          reinterpret_cast<const WAVEFORMATEX&>(requested));
    } else if (closest_match != nullptr) {
      resolved = AudioFormatFromWaveFormat(*closest_match);
      CoTaskMemFree(closest_match);
    } else if (mix_format != nullptr) {
      resolved = AudioFormatFromWaveFormat(*mix_format);
    }
  } else {
    const auto hr = client->IsFormatSupported(
        AUDCLNT_SHAREMODE_EXCLUSIVE, reinterpret_cast<WAVEFORMATEX*>(&requested),
        nullptr);
    if (FAILED(hr)) {
      if (mix_format != nullptr) {
        CoTaskMemFree(mix_format);
      }
      return std::nullopt;
    }
    resolved = AudioFormatFromWaveFormat(
        reinterpret_cast<const WAVEFORMATEX&>(requested));
  }

  if (mix_format != nullptr) {
    CoTaskMemFree(mix_format);
  }
  return resolved;
}

bool WasapiRenderAdapter::ActivateForConfig(const RenderConfig& config,
                                            ComPtr<IMMDevice>* device) {
  *device = ResolveWasapiDevice(eRender, config.device_id);
  return device->Get() != nullptr;
}

WaveCaptureAdapter::~WaveCaptureAdapter() {
  Stop();
}

std::wstring WaveCaptureAdapter::name() const {
  return L"WAVE Capture";
}

AudioBackendType WaveCaptureAdapter::backend_type() const {
  return AudioBackendType::WaveApi;
}

bool WaveCaptureAdapter::SupportsSource(AudioSourceMode source_mode) const {
  return source_mode == AudioSourceMode::MicrophoneCapture;
}

std::vector<AudioDeviceDescriptor> WaveCaptureAdapter::EnumerateDevices(
    AudioSourceMode source_mode) {
  std::vector<AudioDeviceDescriptor> devices;
  if (source_mode == AudioSourceMode::MicrophoneCapture) {
    AudioDeviceDescriptor descriptor;
    descriptor.id = kDefaultWaveId;
    descriptor.friendly_name = L"Default Capture Device";
    descriptor.is_default = true;
    descriptor.direction = AudioDirection::Capture;
    descriptor.capability_flags = kDeviceCapabilityCallbackBuffers;
    devices.push_back(descriptor);
  }

  const auto count = waveInGetNumDevs();
  for (UINT index = 0; index < count; ++index) {
    WAVEINCAPSW caps {};
    if (waveInGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
      continue;
    }
    const bool is_loopback = IsWaveLoopbackName(caps.szPname);
    if (source_mode == AudioSourceMode::SystemLoopback && !is_loopback) {
      continue;
    }
    AudioDeviceDescriptor descriptor;
    descriptor.id = L"wavein:" + std::to_wstring(index);
    descriptor.friendly_name = caps.szPname;
    descriptor.direction = AudioDirection::Capture;
    descriptor.supports_loopback = is_loopback;
    descriptor.capability_flags = kDeviceCapabilityCallbackBuffers;
    devices.push_back(std::move(descriptor));
  }
  return devices;
}

std::optional<AudioFormatSpec> WaveCaptureAdapter::GetPreferredFormat(
    const CaptureConfig& config) {
  loopback_mode_ = config.source_mode == AudioSourceMode::SystemLoopback;
  const auto device_id = ResolveDeviceId(config.device_id);
  if (!device_id.has_value()) {
    last_error_ = L"wave-loopback-device-not-found";
    return std::nullopt;
  }
  return QueryPreferredFormat(config, *device_id);
}

bool WaveCaptureAdapter::Start(const CaptureConfig& config,
                               const AudioFormatSpec& runtime_format,
                               ISessionEventSink* sink) {
  (void)sink;
  Stop();
  last_error_.clear();
  loopback_mode_ = config.source_mode == AudioSourceMode::SystemLoopback;
  runtime_format_ = runtime_format;
  runtime_format_.normalize();
  runtime_wave_format_ = MakeWaveFormatExtensible(runtime_format_);
  runtime_mode_ = L"WAVE API Callback";

  const auto device_id = ResolveDeviceId(config.device_id);
  if (!device_id.has_value()) {
    last_error_ = L"wave-loopback-device-not-found";
    return false;
  }
  const MMRESULT open_result =
      waveInOpen(&wave_in_, *device_id,
                 reinterpret_cast<WAVEFORMATEX*>(&runtime_wave_format_),
                 reinterpret_cast<DWORD_PTR>(&WaveInCallback),
                 reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
  if (open_result != MMSYSERR_NOERROR) {
    last_error_ = L"waveInOpen: " + MmResultToString(open_result);
    wave_in_ = nullptr;
    return false;
  }

  const auto buffer_frames = std::max<uint32_t>(
      runtime_format_.sample_rate * config.buffer_duration_ms / 1000, 480);
  buffers_.resize(4);
  for (size_t index = 0; index < buffers_.size(); ++index) {
    auto& slot = buffers_[index];
    slot.bytes.resize(static_cast<size_t>(buffer_frames) * runtime_wave_format_.Format.nBlockAlign);
    slot.header.lpData = reinterpret_cast<LPSTR>(slot.bytes.data());
    slot.header.dwBufferLength = static_cast<DWORD>(slot.bytes.size());
    slot.header.dwUser = static_cast<DWORD_PTR>(index);
    if (waveInPrepareHeader(wave_in_, &slot.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR ||
        waveInAddBuffer(wave_in_, &slot.header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
      last_error_ = L"waveInPrepareHeader/waveInAddBuffer";
      Stop();
      return false;
    }
  }

  const auto start_result = waveInStart(wave_in_);
  if (start_result != MMSYSERR_NOERROR) {
    last_error_ = L"waveInStart: " + MmResultToString(start_result);
    Stop();
    return false;
  }

  runtime_details_ = L"buffers=" + std::to_wstring(buffers_.size()) +
                     L"; blockAlign=" +
                     std::to_wstring(runtime_wave_format_.Format.nBlockAlign);

  frame_cursor_ = 0;
  return true;
}

void WaveCaptureAdapter::Stop() {
  if (wave_in_) {
    waveInReset(wave_in_);
    for (auto& slot : buffers_) {
      waveInUnprepareHeader(wave_in_, &slot.header, sizeof(WAVEHDR));
    }
    waveInClose(wave_in_);
    wave_in_ = nullptr;
  }
  buffers_.clear();
  std::scoped_lock lock(mutex_);
  ready_indices_.clear();
}

std::optional<AudioFrameChunk> WaveCaptureAdapter::ReadChunk() {
  if (!wave_in_) {
    return std::nullopt;
  }

  size_t buffer_index = 0;
  {
    std::scoped_lock lock(mutex_);
    if (ready_indices_.empty()) {
      return std::nullopt;
    }
    buffer_index = ready_indices_.front();
    ready_indices_.pop_front();
  }

  auto& slot = buffers_[buffer_index];
  const auto frames = static_cast<uint32_t>(
      slot.header.dwBytesRecorded / runtime_wave_format_.Format.nBlockAlign);
  AudioFrameChunk chunk;
  chunk.format = runtime_format_;
  chunk.frame_index = frame_cursor_;
  chunk.first_sample_qpc = 0;
  chunk.interleaved_samples = ConvertPcmToFloat(
      reinterpret_cast<const BYTE*>(slot.header.lpData), frames,
      reinterpret_cast<const WAVEFORMATEX&>(runtime_wave_format_));
  frame_cursor_ += frames;

  slot.header.dwBytesRecorded = 0;
  slot.header.dwFlags &= ~WHDR_DONE;
  waveInAddBuffer(wave_in_, &slot.header, sizeof(WAVEHDR));
  return chunk.interleaved_samples.empty() ? std::nullopt
                                           : std::optional<AudioFrameChunk>(chunk);
}

std::wstring WaveCaptureAdapter::last_error() const {
  return last_error_;
}

std::wstring WaveCaptureAdapter::runtime_mode() const {
  return runtime_mode_;
}

std::wstring WaveCaptureAdapter::runtime_details() const {
  return runtime_details_;
}

void CALLBACK WaveCaptureAdapter::WaveInCallback(HWAVEIN wave_in, UINT message,
                                                 DWORD_PTR instance,
                                                 DWORD_PTR param1,
                                                 DWORD_PTR param2) {
  (void)wave_in;
  (void)param2;
  if (message != WIM_DATA || instance == 0 || param1 == 0) {
    return;
  }
  auto* self = reinterpret_cast<WaveCaptureAdapter*>(instance);
  self->OnWaveInData(reinterpret_cast<WAVEHDR*>(param1));
}

void WaveCaptureAdapter::OnWaveInData(WAVEHDR* header) {
  std::scoped_lock lock(mutex_);
  ready_indices_.push_back(static_cast<size_t>(header->dwUser));
}

std::optional<AudioFormatSpec> WaveCaptureAdapter::QueryPreferredFormat(
    const CaptureConfig& config, UINT device_id) {
  std::vector<AudioFormatSpec> candidates;
  candidates.push_back(ChooseWaveCompatibleFormat(config.format));
  candidates.push_back(AudioFormatSpec{48000, 2, AudioSampleType::PcmInt16, 16, 0, 4, 192000});
  candidates.back().normalize();
  candidates.push_back(AudioFormatSpec{44100, 2, AudioSampleType::PcmInt16, 16, 0, 4, 176400});
  candidates.back().normalize();

  for (auto candidate : candidates) {
    candidate.normalize();
    auto wave = MakeWaveFormatExtensible(candidate);
    if (QueryWaveInFormat(device_id, reinterpret_cast<const WAVEFORMATEX&>(wave)) ==
        MMSYSERR_NOERROR) {
      return candidate;
    }
  }
  return std::nullopt;
}

std::optional<UINT> WaveCaptureAdapter::ResolveDeviceId(
    const std::wstring& device_id) const {
  if (device_id.empty() || device_id == kDefaultWaveId) {
    return loopback_mode_ ? std::nullopt : std::optional<UINT>(WAVE_MAPPER);
  }
  const std::wstring prefix = L"wavein:";
  if (device_id.rfind(prefix, 0) == 0) {
    return static_cast<UINT>(std::stoul(device_id.substr(prefix.size())));
  }
  return loopback_mode_ ? std::nullopt : std::optional<UINT>(WAVE_MAPPER);
}

WaveRenderAdapter::~WaveRenderAdapter() {
  Stop();
}

std::wstring WaveRenderAdapter::name() const {
  return L"WAVE Render";
}

AudioBackendType WaveRenderAdapter::backend_type() const {
  return AudioBackendType::WaveApi;
}

std::vector<AudioDeviceDescriptor> WaveRenderAdapter::EnumerateDevices() {
  std::vector<AudioDeviceDescriptor> devices;
  AudioDeviceDescriptor descriptor;
  descriptor.id = kDefaultWaveId;
  descriptor.friendly_name = L"Default Render Device";
  descriptor.is_default = true;
  descriptor.direction = AudioDirection::Render;
  descriptor.supports_loopback = true;
  descriptor.capability_flags = kDeviceCapabilityCallbackBuffers;
  devices.push_back(descriptor);

  const auto count = waveOutGetNumDevs();
  for (UINT index = 0; index < count; ++index) {
    WAVEOUTCAPSW caps {};
    if (waveOutGetDevCapsW(index, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
      continue;
    }
    AudioDeviceDescriptor item;
    item.id = L"waveout:" + std::to_wstring(index);
    item.friendly_name = caps.szPname;
    item.direction = AudioDirection::Render;
    item.supports_loopback = true;
    item.capability_flags = kDeviceCapabilityCallbackBuffers;
    devices.push_back(std::move(item));
  }
  return devices;
}

std::optional<AudioFormatSpec> WaveRenderAdapter::GetPreferredFormat(
    const RenderConfig& config) {
  return QueryPreferredFormat(config, ResolveDeviceId(config.device_id));
}

bool WaveRenderAdapter::Start(const RenderConfig& config,
                              const AudioFormatSpec& runtime_format,
                              ISessionEventSink* sink) {
  (void)sink;
  Stop();
  last_error_.clear();
  runtime_format_ = runtime_format;
  runtime_format_.normalize();
  runtime_wave_format_ = MakeWaveFormatExtensible(runtime_format_);
  runtime_mode_ = L"WAVE API Callback";

  const auto device_id = ResolveDeviceId(config.device_id);
  const MMRESULT open_result =
      waveOutOpen(&wave_out_, device_id,
                  reinterpret_cast<WAVEFORMATEX*>(&runtime_wave_format_),
                  reinterpret_cast<DWORD_PTR>(&WaveOutCallback),
                  reinterpret_cast<DWORD_PTR>(this), CALLBACK_FUNCTION);
  if (open_result != MMSYSERR_NOERROR) {
    last_error_ = L"waveOutOpen: " + MmResultToString(open_result);
    wave_out_ = nullptr;
    return false;
  }

  const auto buffer_frames = std::max<uint32_t>(
      runtime_format_.sample_rate * config.buffer_duration_ms / 1000,
      runtime_format_.sample_rate / 10);
  buffers_.resize(4);
  for (size_t index = 0; index < buffers_.size(); ++index) {
    auto& slot = buffers_[index];
    slot.bytes.resize(static_cast<size_t>(buffer_frames) *
                      runtime_wave_format_.Format.nBlockAlign);
    slot.header.lpData = reinterpret_cast<LPSTR>(slot.bytes.data());
    slot.header.dwBufferLength = static_cast<DWORD>(slot.bytes.size());
    slot.header.dwUser = static_cast<DWORD_PTR>(index);
    if (waveOutPrepareHeader(wave_out_, &slot.header, sizeof(WAVEHDR)) !=
        MMSYSERR_NOERROR) {
      last_error_ = L"waveOutPrepareHeader";
      Stop();
      return false;
    }
    free_indices_.push_back(index);
  }
  runtime_details_ = L"buffers=" + std::to_wstring(buffers_.size()) +
                     L"; blockAlign=" +
                     std::to_wstring(runtime_wave_format_.Format.nBlockAlign);
  return true;
}

void WaveRenderAdapter::Stop() {
  if (wave_out_) {
    waveOutReset(wave_out_);
    for (auto& slot : buffers_) {
      waveOutUnprepareHeader(wave_out_, &slot.header, sizeof(WAVEHDR));
    }
    waveOutClose(wave_out_);
    wave_out_ = nullptr;
  }
  buffers_.clear();
  std::scoped_lock lock(mutex_);
  free_indices_.clear();
}

bool WaveRenderAdapter::WriteChunk(const AudioFrameChunk& chunk) {
  if (!wave_out_) {
    last_error_ = L"waveOut not started";
    return false;
  }

  size_t buffer_index = 0;
  {
    std::scoped_lock lock(mutex_);
    if (free_indices_.empty()) {
      last_error_ = L"waveOut buffer queue exhausted";
      return false;
    }
    buffer_index = free_indices_.front();
    free_indices_.pop_front();
  }

  auto& slot = buffers_[buffer_index];
  const auto capacity_frames = static_cast<uint32_t>(
      slot.bytes.size() / runtime_wave_format_.Format.nBlockAlign);
  const auto frames_to_write = std::min<uint32_t>(capacity_frames, chunk.frame_count());
  std::vector<BYTE> pcm_bytes;
  ConvertFloatToPcm(chunk.interleaved_samples.data(), frames_to_write,
                    reinterpret_cast<const WAVEFORMATEX&>(runtime_wave_format_),
                    &pcm_bytes);
  std::memcpy(slot.bytes.data(), pcm_bytes.data(), pcm_bytes.size());
  slot.header.dwBufferLength = static_cast<DWORD>(pcm_bytes.size());
  const auto write_result = waveOutWrite(wave_out_, &slot.header, sizeof(WAVEHDR));
  if (write_result != MMSYSERR_NOERROR) {
    last_error_ = L"waveOutWrite: " + MmResultToString(write_result);
    return false;
  }
  return true;
}

std::wstring WaveRenderAdapter::last_error() const {
  return last_error_;
}

std::wstring WaveRenderAdapter::runtime_mode() const {
  return runtime_mode_;
}

std::wstring WaveRenderAdapter::runtime_details() const {
  return runtime_details_;
}

void CALLBACK WaveRenderAdapter::WaveOutCallback(HWAVEOUT wave_out, UINT message,
                                                 DWORD_PTR instance,
                                                 DWORD_PTR param1,
                                                 DWORD_PTR param2) {
  (void)wave_out;
  (void)param2;
  if (message != WOM_DONE || instance == 0 || param1 == 0) {
    return;
  }
  auto* self = reinterpret_cast<WaveRenderAdapter*>(instance);
  self->OnWaveOutDone(reinterpret_cast<WAVEHDR*>(param1));
}

void WaveRenderAdapter::OnWaveOutDone(WAVEHDR* header) {
  std::scoped_lock lock(mutex_);
  free_indices_.push_back(static_cast<size_t>(header->dwUser));
}

std::optional<AudioFormatSpec> WaveRenderAdapter::QueryPreferredFormat(
    const RenderConfig& config, UINT device_id) {
  std::vector<AudioFormatSpec> candidates;
  candidates.push_back(ChooseWaveCompatibleFormat(config.format));
  candidates.push_back(AudioFormatSpec{48000, 2, AudioSampleType::PcmInt16, 16, 0, 4, 192000});
  candidates.back().normalize();
  candidates.push_back(AudioFormatSpec{44100, 2, AudioSampleType::PcmInt16, 16, 0, 4, 176400});
  candidates.back().normalize();

  for (auto candidate : candidates) {
    candidate.normalize();
    auto wave = MakeWaveFormatExtensible(candidate);
    if (QueryWaveOutFormat(device_id, reinterpret_cast<const WAVEFORMATEX&>(wave)) ==
        MMSYSERR_NOERROR) {
      return candidate;
    }
  }
  return std::nullopt;
}

UINT WaveRenderAdapter::ResolveDeviceId(const std::wstring& device_id) const {
  if (device_id.empty() || device_id == kDefaultWaveId) {
    return WAVE_MAPPER;
  }
  const std::wstring prefix = L"waveout:";
  if (device_id.rfind(prefix, 0) == 0) {
    return static_cast<UINT>(std::stoul(device_id.substr(prefix.size())));
  }
  return WAVE_MAPPER;
}

std::unique_ptr<IAudioCaptureAdapter> RealAudioBackendFactory::CreateCaptureAdapter(
    AudioBackendType backend) {
  if (backend == AudioBackendType::Wasapi) {
    return std::make_unique<WasapiCaptureAdapter>();
  }
  return std::make_unique<WaveCaptureAdapter>();
}

std::unique_ptr<IAudioRenderAdapter> RealAudioBackendFactory::CreateRenderAdapter(
    AudioBackendType backend) {
  if (backend == AudioBackendType::Wasapi) {
    return std::make_unique<WasapiRenderAdapter>();
  }
  return std::make_unique<WaveRenderAdapter>();
}

}  // namespace winaudio
