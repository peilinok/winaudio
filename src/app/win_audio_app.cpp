#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#include <memory>
#include <string>
#include <thread>

#include "app_model.h"
#include "probe_ui_text.h"
#include "device_notification_client.h"
#include "audio/com_support.h"
#include "ui/waveform_renderer.h"

namespace winaudio {

namespace {

constexpr wchar_t kWindowClassName[] = L"WinAudioDemoWindowClass";
constexpr UINT_PTR kUiTimerId = 1;
constexpr UINT_PTR kAudioTimerId = 2;
constexpr UINT kMessageProbeFinished = WM_APP + 2;
constexpr UINT kMessageProbeMatrixFinished = WM_APP + 3;

constexpr int kButtonStart = 1001;
constexpr int kButtonStop = 1002;
constexpr int kButtonRefresh = 1003;
constexpr int kButtonProbe = 1004;
constexpr int kButtonProbeMatrix = 1005;
constexpr int kComboCaptureBackend = 1101;
constexpr int kComboRenderBackend = 1102;
constexpr int kComboSourceMode = 1103;
constexpr int kComboCaptureDevice = 1104;
constexpr int kComboRenderDevice = 1105;
constexpr int kEditDelayMs = 1106;
constexpr int kCheckboxDump = 1107;
constexpr int kComboCaptureSampleRate = 1108;
constexpr int kComboCaptureChannels = 1109;
constexpr int kComboCaptureSampleType = 1110;
constexpr int kComboRenderSampleRate = 1119;
constexpr int kComboRenderChannels = 1120;
constexpr int kComboRenderSampleType = 1121;
constexpr int kComboCaptureShareMode = 1111;
constexpr int kComboCaptureDriveMode = 1112;
constexpr int kComboRenderShareMode = 1123;
constexpr int kComboRenderDriveMode = 1124;
constexpr int kEditDumpPath = 1113;
constexpr int kComboDumpType = 1114;
constexpr int kEditCaptureBufferMs = 1115;
constexpr int kEditRenderBufferMs = 1116;
constexpr int kCheckboxMonitor = 1117;
constexpr int kCheckboxFollowDefaults = 1118;
constexpr int kCheckboxAutoAlignRender = 1122;
constexpr int kAutomationCaptureLabel = 1901;
constexpr int kAutomationDeviceCountLine = 1902;
constexpr int kAutomationSummaryText = 1903;
constexpr int kAutomationDiagnosticsText = 1904;
constexpr int kAutomationProbeText = 1905;
constexpr int kAutomationAutoAlignNote = 1906;

struct WindowContext {
  ScopedCoInitialize com {};
  AppModel model {};
  HWND start_button = nullptr;
  HWND stop_button = nullptr;
  HWND refresh_button = nullptr;
  HWND probe_button = nullptr;
  HWND probe_matrix_button = nullptr;
  HWND capture_backend_combo = nullptr;
  HWND render_backend_combo = nullptr;
  HWND source_mode_combo = nullptr;
  HWND capture_device_combo = nullptr;
  HWND render_device_combo = nullptr;
  HWND delay_edit = nullptr;
  HWND dump_checkbox = nullptr;
  HWND capture_sample_rate_combo = nullptr;
  HWND capture_channels_combo = nullptr;
  HWND capture_sample_type_combo = nullptr;
  HWND render_sample_rate_combo = nullptr;
  HWND render_channels_combo = nullptr;
  HWND render_sample_type_combo = nullptr;
  HWND capture_share_mode_combo = nullptr;
  HWND capture_drive_mode_combo = nullptr;
  HWND render_share_mode_combo = nullptr;
  HWND render_drive_mode_combo = nullptr;
  HWND dump_path_edit = nullptr;
  HWND dump_type_combo = nullptr;
  HWND capture_buffer_edit = nullptr;
  HWND render_buffer_edit = nullptr;
  HWND monitor_checkbox = nullptr;
  HWND follow_defaults_checkbox = nullptr;
  HWND auto_align_render_checkbox = nullptr;
  HWND automation_capture_label = nullptr;
  HWND automation_device_count_line = nullptr;
  HWND automation_summary_text = nullptr;
  HWND automation_diagnostics_text = nullptr;
  HWND automation_probe_text = nullptr;
  HWND automation_auto_align_note = nullptr;
  DeviceNotificationClient* device_notifications = nullptr;
  bool probe_running = false;
  ProbeUiMode probe_mode = ProbeUiMode::None;
  std::jthread probe_thread {};
  bool shutting_down = false;
};

HMENU ControlIdToMenu(int id) {
  return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

void SetControlText(HWND hwnd, const wchar_t* text) {
  SetWindowTextW(hwnd, text);
}

void ApplyControlAvailability(WindowContext* context) {
  const auto config = context->model.configuration();
  const auto session_state = context->model.session_state();
  const bool busy = context->probe_running;
  const bool monitor_enabled = config.render.monitor_enabled;
  const BOOL general_enabled = busy ? FALSE : TRUE;
  const BOOL capture_device_combo_enabled =
      (!busy && !config.follow_default_devices) ? TRUE : FALSE;
  const BOOL render_pipeline_enabled =
      (!busy && monitor_enabled) ? TRUE : FALSE;
  const BOOL render_device_combo_enabled =
      (!busy && monitor_enabled && !config.follow_default_devices) ? TRUE : FALSE;
  const BOOL render_format_enabled =
      (!busy && monitor_enabled && !config.auto_align_render_format) ? TRUE : FALSE;
  const BOOL start_enabled =
      (!busy && session_state != L"Running") ? TRUE : FALSE;
  const BOOL stop_enabled =
      (!busy && session_state == L"Running") ? TRUE : FALSE;

  if (context->start_button != nullptr) {
    EnableWindow(context->start_button, start_enabled);
  }
  if (context->stop_button != nullptr) {
    EnableWindow(context->stop_button, stop_enabled);
  }

  const HWND general_controls[] = {
      context->refresh_button,
      context->probe_button,
      context->probe_matrix_button,
      context->capture_backend_combo,
      context->source_mode_combo,
      context->dump_checkbox,
      context->capture_sample_rate_combo,
      context->capture_channels_combo,
      context->capture_sample_type_combo,
      context->capture_share_mode_combo,
      context->capture_drive_mode_combo,
      context->dump_path_edit,
      context->dump_type_combo,
      context->capture_buffer_edit,
      context->monitor_checkbox,
      context->follow_defaults_checkbox,
  };
  for (HWND control : general_controls) {
    if (control != nullptr) {
      EnableWindow(control, general_enabled);
    }
  }
  if (context->capture_device_combo != nullptr) {
    EnableWindow(context->capture_device_combo, capture_device_combo_enabled);
  }
  if (context->render_device_combo != nullptr) {
    EnableWindow(context->render_device_combo, render_device_combo_enabled);
  }
  const HWND render_pipeline_controls[] = {
      context->render_backend_combo,
      context->delay_edit,
      context->render_share_mode_combo,
      context->render_drive_mode_combo,
      context->render_buffer_edit,
      context->auto_align_render_checkbox,
  };
  for (HWND control : render_pipeline_controls) {
    if (control != nullptr) {
      EnableWindow(control, render_pipeline_enabled);
    }
  }
  if (context->render_sample_rate_combo != nullptr) {
    EnableWindow(context->render_sample_rate_combo, render_format_enabled);
  }
  if (context->render_channels_combo != nullptr) {
    EnableWindow(context->render_channels_combo, render_format_enabled);
  }
  if (context->render_sample_type_combo != nullptr) {
    EnableWindow(context->render_sample_type_combo, render_format_enabled);
  }
}

void SetProbeUiBusy(WindowContext* context, bool busy) {
  context->probe_running = busy;
  ApplyControlAvailability(context);
  SetWindowTextW(context->probe_button, BuildProbeButtonLabel(busy).c_str());
  SetWindowTextW(context->probe_matrix_button,
                 BuildProbeMatrixButtonLabel(busy).c_str());
}

void SyncAutomationTextMirrors(WindowContext* context) {
  if (context == nullptr) {
    return;
  }

  const auto devices = context->model.devices();
  const bool loopback_source =
      context->model.configuration().capture.source_mode ==
      AudioSourceMode::SystemLoopback;

  if (context->automation_capture_label != nullptr) {
    SetWindowTextW(context->automation_capture_label,
                   BuildCaptureDeviceLabelText(loopback_source).c_str());
  }
  if (context->automation_device_count_line != nullptr) {
    SetWindowTextW(
        context->automation_device_count_line,
        BuildDeviceCountLineText(loopback_source, devices.capture_devices.size(),
                                 devices.render_devices.size())
            .c_str());
  }
  if (context->automation_summary_text != nullptr) {
    SetWindowTextW(context->automation_summary_text,
                   context->model.summary_text().c_str());
  }
  if (context->automation_diagnostics_text != nullptr) {
    SetWindowTextW(context->automation_diagnostics_text,
                   context->model.diagnostics_text().c_str());
  }
  if (context->automation_probe_text != nullptr) {
    SetWindowTextW(context->automation_probe_text,
                   context->model.probe_text().c_str());
  }
  if (context->automation_auto_align_note != nullptr) {
    SetWindowTextW(context->automation_auto_align_note,
                   BuildAutoAlignExplanatoryNoteText().c_str());
  }
}

std::wstring GetWindowTextString(HWND hwnd) {
  const int length = GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<size_t>(length), L'\0');
  if (length > 0) {
    GetWindowTextW(hwnd, text.data(), length + 1);
  }
  return text;
}

void PopulateBackendCombo(HWND combo) {
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WASAPI"));
  SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"WAVE API"));
}

void PopulateSourceModeCombo(HWND combo) {
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Microphone"));
  SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"System Loopback"));
}

void PopulateSimpleCombo(HWND combo, const std::vector<std::wstring>& items) {
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  for (const auto& item : items) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
  }
}

uint32_t SampleRateFromComboIndex(int index) {
  switch (index) {
    case 0:
      return 8000;
    case 1:
      return 16000;
    case 2:
      return 32000;
    case 3:
      return 44100;
    case 4:
      return 48000;
    case 5:
      return 96000;
    default:
      return 48000;
  }
}

int ComboIndexFromSampleRate(uint32_t sample_rate) {
  switch (sample_rate) {
    case 8000:
      return 0;
    case 16000:
      return 1;
    case 32000:
      return 2;
    case 44100:
      return 3;
    case 48000:
      return 4;
    case 96000:
      return 5;
    default:
      return 4;
  }
}

uint16_t ChannelsFromComboIndex(int index) {
  switch (index) {
    case 0:
      return 1;
    case 1:
      return 2;
    case 2:
      return 4;
    case 3:
      return 6;
    case 4:
      return 8;
    default:
      return 2;
  }
}

int ComboIndexFromChannels(uint16_t channels) {
  switch (channels) {
    case 1:
      return 0;
    case 2:
      return 1;
    case 4:
      return 2;
    case 6:
      return 3;
    case 8:
      return 4;
    default:
      return 1;
  }
}

AudioSampleType SampleTypeFromComboIndex(int index) {
  switch (index) {
    case 0:
      return AudioSampleType::PcmInt16;
    case 1:
      return AudioSampleType::PcmInt24;
    case 2:
      return AudioSampleType::PcmInt32;
    default:
      return AudioSampleType::Float32;
  }
}

int ComboIndexFromSampleType(AudioSampleType sample_type) {
  switch (sample_type) {
    case AudioSampleType::PcmInt16:
      return 0;
    case AudioSampleType::PcmInt24:
      return 1;
    case AudioSampleType::PcmInt32:
      return 2;
    case AudioSampleType::Float32:
      return 3;
  }
  return 0;
}

void PopulateDeviceCombo(HWND combo,
                         const std::vector<AudioDeviceDescriptor>& devices,
                         const std::wstring& selected_device_id) {
  SendMessageW(combo, CB_RESETCONTENT, 0, 0);
  int default_index = -1;
  int selected_index = -1;
  for (const auto& device : devices) {
    const auto index = static_cast<int>(
        SendMessageW(combo, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(device.friendly_name.c_str())));
    if (index >= 0) {
      auto* value = new std::wstring(device.id);
      SendMessageW(combo, CB_SETITEMDATA, index,
                   reinterpret_cast<LPARAM>(value));
      if (device.is_default) {
        default_index = index;
      }
      if (!selected_device_id.empty() && device.id == selected_device_id) {
        selected_index = index;
      }
    }
  }
  if (selected_index >= 0) {
    SendMessageW(combo, CB_SETCURSEL, selected_index, 0);
  } else if (default_index >= 0) {
    SendMessageW(combo, CB_SETCURSEL, default_index, 0);
  } else if (!devices.empty() && SendMessageW(combo, CB_GETCURSEL, 0, 0) == CB_ERR) {
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
  }
}

void ClearDeviceComboHeapStrings(HWND combo) {
  const auto count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
  for (int index = 0; index < count; ++index) {
    auto* value = reinterpret_cast<std::wstring*>(
        SendMessageW(combo, CB_GETITEMDATA, index, 0));
    if (value != nullptr && value != reinterpret_cast<std::wstring*>(CB_ERR)) {
      delete value;
    }
  }
}

std::wstring GetSelectedComboDeviceId(HWND combo) {
  const auto index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
  if (index == CB_ERR) {
    return {};
  }
  auto* value = reinterpret_cast<std::wstring*>(
      SendMessageW(combo, CB_GETITEMDATA, index, 0));
  return value != nullptr ? *value : std::wstring {};
}

void SelectComboDeviceId(HWND combo, const std::wstring& device_id) {
  if (device_id.empty()) {
    return;
  }
  const auto count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
  for (int index = 0; index < count; ++index) {
    auto* value = reinterpret_cast<std::wstring*>(
        SendMessageW(combo, CB_GETITEMDATA, index, 0));
    if (value != nullptr && value != reinterpret_cast<std::wstring*>(CB_ERR) &&
        *value == device_id) {
      SendMessageW(combo, CB_SETCURSEL, index, 0);
      return;
    }
  }
}

void SyncUiFromModel(WindowContext* context) {
  const auto config = context->model.configuration();
  const auto devices = context->model.devices();

  SendMessageW(context->capture_backend_combo, CB_SETCURSEL,
               config.capture.backend == AudioBackendType::Wasapi ? 0 : 1, 0);
  SendMessageW(context->render_backend_combo, CB_SETCURSEL,
               config.render.backend == AudioBackendType::Wasapi ? 0 : 1, 0);
  SendMessageW(context->source_mode_combo, CB_SETCURSEL,
               config.capture.source_mode == AudioSourceMode::MicrophoneCapture ? 0
                                                                                : 1,
               0);
  ClearDeviceComboHeapStrings(context->capture_device_combo);
  ClearDeviceComboHeapStrings(context->render_device_combo);
  PopulateDeviceCombo(context->capture_device_combo, devices.capture_devices,
                      config.capture.device_id);
  PopulateDeviceCombo(context->render_device_combo, devices.render_devices,
                      config.render.device_id);
  SelectComboDeviceId(context->capture_device_combo, config.capture.device_id);
  SelectComboDeviceId(context->render_device_combo, config.render.device_id);

  const auto delay = std::to_wstring(config.render.fixed_delay_ms);
  SetControlText(context->delay_edit, delay.c_str());
  SendMessageW(context->dump_checkbox, BM_SETCHECK,
               config.capture.dump_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(context->capture_sample_rate_combo, CB_SETCURSEL,
               ComboIndexFromSampleRate(config.capture.format.sample_rate), 0);
  SendMessageW(context->capture_channels_combo, CB_SETCURSEL,
               ComboIndexFromChannels(config.capture.format.channels), 0);
  SendMessageW(context->capture_sample_type_combo, CB_SETCURSEL,
               ComboIndexFromSampleType(config.capture.format.sample_type),
               0);
  SendMessageW(context->render_sample_rate_combo, CB_SETCURSEL,
               ComboIndexFromSampleRate(config.render.format.sample_rate), 0);
  SendMessageW(context->render_channels_combo, CB_SETCURSEL,
               ComboIndexFromChannels(config.render.format.channels), 0);
  SendMessageW(context->render_sample_type_combo, CB_SETCURSEL,
               ComboIndexFromSampleType(config.render.format.sample_type),
               0);
  SendMessageW(context->capture_share_mode_combo, CB_SETCURSEL,
               config.capture.wasapi_share_mode == WasapiShareMode::Shared ? 0 : 1,
               0);
  SendMessageW(context->capture_drive_mode_combo, CB_SETCURSEL,
               config.capture.wasapi_drive_mode == WasapiDriveMode::EventDriven ? 0
                                                                                : 1,
               0);
  SendMessageW(context->render_share_mode_combo, CB_SETCURSEL,
               config.render.wasapi_share_mode == WasapiShareMode::Shared ? 0 : 1,
               0);
  SendMessageW(context->render_drive_mode_combo, CB_SETCURSEL,
               config.render.wasapi_drive_mode == WasapiDriveMode::EventDriven ? 0
                                                                               : 1,
               0);
  SetControlText(context->dump_path_edit, config.capture.dump_path.c_str());
  SendMessageW(context->dump_type_combo, CB_SETCURSEL,
               config.capture.dump_file_type == DumpFileType::Wav ? 0 : 1, 0);
  SetControlText(context->capture_buffer_edit,
                 std::to_wstring(config.capture.buffer_duration_ms).c_str());
  SetControlText(context->render_buffer_edit,
                 std::to_wstring(config.render.buffer_duration_ms).c_str());
  SendMessageW(context->monitor_checkbox, BM_SETCHECK,
               config.render.monitor_enabled ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(context->follow_defaults_checkbox, BM_SETCHECK,
               config.follow_default_devices ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageW(context->auto_align_render_checkbox, BM_SETCHECK,
               config.auto_align_render_format ? BST_CHECKED : BST_UNCHECKED, 0);
  ApplyControlAvailability(context);
  SyncAutomationTextMirrors(context);
}

void DrawConfigLabels(HDC hdc) {
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(45, 50, 56));
  TextOutW(hdc, 16, 56, L"Capture Backend", 15);
  TextOutW(hdc, 220, 56, L"Render Backend", 14);
  TextOutW(hdc, 424, 56, L"Source Mode", 11);
  TextOutW(hdc, 16, 106, L"Capture Device", 14);
  TextOutW(hdc, 560, 106, L"Render Device", 13);
  TextOutW(hdc, 16, 156, L"Fixed Delay (ms)", 16);
  TextOutW(hdc, 320, 156, L"Capture Rate", 12);
  TextOutW(hdc, 470, 156, L"Capture Ch", 10);
  TextOutW(hdc, 600, 156, L"Capture Type", 12);
  TextOutW(hdc, 320, 206, L"Render Rate", 11);
  TextOutW(hdc, 470, 206, L"Render Ch", 9);
  TextOutW(hdc, 600, 206, L"Render Type", 11);
  TextOutW(hdc, 760, 156, L"Cap Share", 9);
  TextOutW(hdc, 920, 156, L"Cap Drive", 9);
  TextOutW(hdc, 760, 206, L"Ren Share", 9);
  TextOutW(hdc, 920, 206, L"Ren Drive", 9);
  TextOutW(hdc, 16, 256, L"Dump Path", 9);
  TextOutW(hdc, 860, 256, L"Dump Type", 9);
  TextOutW(hdc, 16, 306, L"Capture Buffer (ms)", 19);
  TextOutW(hdc, 220, 306, L"Render Buffer (ms)", 18);
  TextOutW(hdc, 424, 306, L"Monitor Playback", 16);
  TextOutW(hdc, 700, 306, L"Default Device Follow", 21);
  TextOutW(hdc, 700, 336, L"Render Auto Align", 17);
  const auto auto_align_note = BuildAutoAlignExplanatoryNoteText();
  TextOutW(hdc, 16, 366, auto_align_note.c_str(),
           static_cast<int>(auto_align_note.size()));
}

void DrawConfigLabels(HDC hdc, WindowContext* context) {
  DrawConfigLabels(hdc);
  if (context == nullptr) {
    return;
  }
  const bool loopback_source =
      context->model.configuration().capture.source_mode ==
      AudioSourceMode::SystemLoopback;
  if (!loopback_source) {
    return;
  }
  const auto label = BuildCaptureDeviceLabelText(true);
  RECT cover = {16, 106, 220, 124};
  HBRUSH background = CreateSolidBrush(GetSysColor(COLOR_WINDOW));
  FillRect(hdc, &cover, background);
  DeleteObject(background);
  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(45, 50, 56));
  TextOutW(hdc, 16, 106, label.c_str(), static_cast<int>(label.size()));
}

void UpdateWindowTitle(HWND hwnd, WindowContext* context) {
  const auto stats = context->model.stats();
  const auto config = context->model.configuration();
  const auto capture_title_format = BuildWindowTitleFormatText(
      stats.negotiated_capture_format,
      DescribeAudioFormat(config.capture.format));
  const auto render_title_format = BuildWindowTitleFormatText(
      stats.negotiated_render_format,
      DescribeAudioFormat(config.render.format));
  const auto title = BuildWindowTitleText(
      context->probe_mode, context->model.session_state(),
      capture_title_format, render_title_format);
  SetWindowTextW(hwnd, title.c_str());
}

void DrawSummary(HDC hdc, const RECT& rect, WindowContext* context) {
  HBRUSH background = CreateSolidBrush(RGB(245, 247, 250));
  FillRect(hdc, &rect, background);
  DeleteObject(background);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(28, 32, 36));

  const auto stats = context->model.stats();
  const auto logs = context->model.logs();
  const auto devices = context->model.devices();
  const auto summary = context->model.summary_text();
  const auto diagnostics = context->model.diagnostics_text();

  std::wstring line1 = L"State: " + context->model.session_state();
  const bool loopback_source =
      context->model.configuration().capture.source_mode ==
      AudioSourceMode::SystemLoopback;
  std::wstring line2 = BuildDeviceCountLineText(loopback_source,
                                                devices.capture_devices.size(),
                                                devices.render_devices.size());
  const auto loopback_note = BuildLoopbackCaptureNoteText(loopback_source);
  std::wstring line3 = L"Queue: " + std::to_wstring(stats.queue_depth_ms) +
                       L" ms | Delay estimate: " +
                       std::to_wstring(stats.estimated_monitor_delay_ms) + L" ms";
  std::wstring line4 = L"Dropped: " + std::to_wstring(stats.dropped_frames) +
                       L" | Underruns: " + std::to_wstring(stats.render_underruns);

  TextOutW(hdc, rect.left + 16, rect.top + 12, line1.c_str(),
           static_cast<int>(line1.size()));
  TextOutW(hdc, rect.left + 16, rect.top + 32, line2.c_str(),
           static_cast<int>(line2.size()));
  const int line3_y = loopback_note.empty() ? rect.top + 52 : rect.top + 68;
  const int line4_y = loopback_note.empty() ? rect.top + 72 : rect.top + 88;
  if (!loopback_note.empty()) {
    TextOutW(hdc, rect.left + 16, rect.top + 50, loopback_note.c_str(),
             static_cast<int>(loopback_note.size()));
  }
  TextOutW(hdc, rect.left + 16, line3_y, line3.c_str(),
           static_cast<int>(line3.size()));
  TextOutW(hdc, rect.left + 16, line4_y, line4.c_str(),
           static_cast<int>(line4.size()));
  RECT summary_rect = {rect.left + 340, rect.top + 8, rect.right - 16, rect.top + 94};
  DrawTextW(hdc, summary.c_str(), -1, &summary_rect, DT_LEFT | DT_WORDBREAK);
  RECT diagnostics_rect = {rect.left + 340, rect.top + 92, rect.right - 16,
                           rect.top + 156};
  DrawTextW(hdc, diagnostics.c_str(), -1, &diagnostics_rect,
            DT_LEFT | DT_WORDBREAK);

  const int log_top = rect.top + 104;
  TextOutW(hdc, rect.left + 16, log_top, L"Recent Logs", 11);
  int line_y = log_top + 22;
  for (auto it = logs.rbegin(); it != logs.rend() && line_y < rect.bottom - 8; ++it) {
    TextOutW(hdc, rect.left + 16, line_y, it->c_str(),
             static_cast<int>(it->size()));
    line_y += 18;
  }
}

void DrawCapabilityPanel(HDC hdc, const RECT& rect, WindowContext* context) {
  HBRUSH background = CreateSolidBrush(RGB(236, 241, 247));
  FillRect(hdc, &rect, background);
  DeleteObject(background);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(34, 40, 49));
  TextOutW(hdc, rect.left + 16, rect.top + 10, L"Capability Panel", 16);

  RECT text_rect = {rect.left + 16, rect.top + 34, rect.right - 16,
                    rect.bottom - 10};
  const auto capability = context->model.capability_text();
  DrawTextW(hdc, capability.c_str(), -1, &text_rect, DT_LEFT | DT_WORDBREAK);
}

void DrawProbePanel(HDC hdc, const RECT& rect, WindowContext* context) {
  HBRUSH background = CreateSolidBrush(RGB(230, 238, 246));
  FillRect(hdc, &rect, background);
  DeleteObject(background);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(34, 40, 49));
  TextOutW(hdc, rect.left + 16, rect.top + 10, L"Probe Panel", 11);

  RECT text_rect = {rect.left + 16, rect.top + 34, rect.right - 16,
                    rect.bottom - 10};
  const auto probe = context->model.probe_text();
  DrawTextW(hdc, probe.c_str(), -1, &text_rect, DT_LEFT | DT_WORDBREAK);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM w_param,
                            LPARAM l_param) {
  auto* context =
      reinterpret_cast<WindowContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

  switch (message) {
    case WM_CREATE: {
      auto* create_struct = reinterpret_cast<LPCREATESTRUCTW>(l_param);
      auto* owned_context =
          reinterpret_cast<WindowContext*>(create_struct->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                        reinterpret_cast<LONG_PTR>(owned_context));

      owned_context->start_button = CreateWindowW(
          L"BUTTON", L"Start Session", WS_TABSTOP | WS_VISIBLE | WS_CHILD,
          16, 16, 120, 28, hwnd, ControlIdToMenu(kButtonStart), nullptr,
          nullptr);
      owned_context->stop_button = CreateWindowW(
          L"BUTTON", L"Stop", WS_TABSTOP | WS_VISIBLE | WS_CHILD, 148, 16, 90, 28,
          hwnd, ControlIdToMenu(kButtonStop), nullptr, nullptr);
      owned_context->refresh_button = CreateWindowW(
          L"BUTTON", L"Refresh Devices", WS_TABSTOP | WS_VISIBLE | WS_CHILD, 250,
          16, 130, 28, hwnd, ControlIdToMenu(kButtonRefresh), nullptr, nullptr);
      owned_context->probe_button = CreateWindowW(
          L"BUTTON", L"Run Quick Probe", WS_TABSTOP | WS_VISIBLE | WS_CHILD, 392,
          16, 140, 28, hwnd, ControlIdToMenu(kButtonProbe), nullptr, nullptr);
      owned_context->probe_matrix_button = CreateWindowW(
          L"BUTTON", L"Run Probe Matrix", WS_TABSTOP | WS_VISIBLE | WS_CHILD, 544,
          16, 150, 28, hwnd, ControlIdToMenu(kButtonProbeMatrix), nullptr,
          nullptr);
      owned_context->capture_backend_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 16, 76,
          180, 200, hwnd, ControlIdToMenu(kComboCaptureBackend), nullptr,
          nullptr);
      owned_context->render_backend_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 220, 76,
          180, 200, hwnd, ControlIdToMenu(kComboRenderBackend), nullptr,
          nullptr);
      owned_context->source_mode_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 424, 76,
          120, 200, hwnd, ControlIdToMenu(kComboSourceMode), nullptr,
          nullptr);
      owned_context->capture_device_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 16, 126,
          520, 250, hwnd, ControlIdToMenu(kComboCaptureDevice), nullptr,
          nullptr);
      owned_context->render_device_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 560, 126,
          520, 250, hwnd, ControlIdToMenu(kComboRenderDevice), nullptr,
          nullptr);
      owned_context->delay_edit = CreateWindowW(
          L"EDIT", L"120", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 16,
          176, 100, 24, hwnd, ControlIdToMenu(kEditDelayMs), nullptr,
          nullptr);
      owned_context->dump_checkbox = CreateWindowW(
          L"BUTTON", L"Enable PCM Dump", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
          140, 176, 160, 24, hwnd, ControlIdToMenu(kCheckboxDump), nullptr,
          nullptr);
      owned_context->capture_sample_rate_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 320, 176,
          120, 180, hwnd, ControlIdToMenu(kComboCaptureSampleRate), nullptr,
          nullptr);
      owned_context->capture_channels_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 470, 176,
          100, 180, hwnd, ControlIdToMenu(kComboCaptureChannels), nullptr,
          nullptr);
      owned_context->capture_sample_type_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 600, 176,
          130, 180, hwnd, ControlIdToMenu(kComboCaptureSampleType), nullptr,
          nullptr);
      owned_context->render_sample_rate_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 320, 226,
          120, 180, hwnd, ControlIdToMenu(kComboRenderSampleRate), nullptr,
          nullptr);
      owned_context->render_channels_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 470, 226,
          100, 180, hwnd, ControlIdToMenu(kComboRenderChannels), nullptr,
          nullptr);
      owned_context->render_sample_type_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 600, 226,
          130, 180, hwnd, ControlIdToMenu(kComboRenderSampleType), nullptr,
          nullptr);
      owned_context->capture_share_mode_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 760, 176,
          130, 180, hwnd, ControlIdToMenu(kComboCaptureShareMode), nullptr,
          nullptr);
      owned_context->capture_drive_mode_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 920, 176,
          130, 180, hwnd, ControlIdToMenu(kComboCaptureDriveMode), nullptr,
          nullptr);
      owned_context->render_share_mode_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 760, 226,
          130, 180, hwnd, ControlIdToMenu(kComboRenderShareMode), nullptr,
          nullptr);
      owned_context->render_drive_mode_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 920, 226,
          130, 180, hwnd, ControlIdToMenu(kComboRenderDriveMode), nullptr,
          nullptr);
      owned_context->dump_path_edit = CreateWindowW(
          L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 16, 276,
          820, 24, hwnd, ControlIdToMenu(kEditDumpPath), nullptr, nullptr);
      owned_context->dump_type_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 860, 276,
          190, 180, hwnd, ControlIdToMenu(kComboDumpType), nullptr, nullptr);
      owned_context->capture_buffer_edit = CreateWindowW(
          L"EDIT", L"40", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 16,
          326, 160, 24, hwnd, ControlIdToMenu(kEditCaptureBufferMs), nullptr,
          nullptr);
      owned_context->render_buffer_edit = CreateWindowW(
          L"EDIT", L"40", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 220,
          326, 160, 24, hwnd, ControlIdToMenu(kEditRenderBufferMs), nullptr,
          nullptr);
      owned_context->monitor_checkbox = CreateWindowW(
          L"BUTTON", L"Enable monitor playback",
          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 424, 326, 220, 24, hwnd,
          ControlIdToMenu(kCheckboxMonitor), nullptr, nullptr);
      owned_context->follow_defaults_checkbox = CreateWindowW(
          L"BUTTON", L"Follow default devices",
          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 700, 326, 220, 24, hwnd,
          ControlIdToMenu(kCheckboxFollowDefaults), nullptr, nullptr);
      owned_context->auto_align_render_checkbox = CreateWindowW(
          L"BUTTON", L"Auto align render format",
          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 700, 356, 220, 24, hwnd,
          ControlIdToMenu(kCheckboxAutoAlignRender), nullptr, nullptr);
      owned_context->automation_capture_label = CreateWindowW(
          L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, hwnd,
          ControlIdToMenu(kAutomationCaptureLabel), nullptr, nullptr);
      owned_context->automation_device_count_line = CreateWindowW(
          L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, hwnd,
          ControlIdToMenu(kAutomationDeviceCountLine), nullptr, nullptr);
      owned_context->automation_summary_text = CreateWindowW(
          L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, hwnd,
          ControlIdToMenu(kAutomationSummaryText), nullptr, nullptr);
      owned_context->automation_diagnostics_text = CreateWindowW(
          L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, hwnd,
          ControlIdToMenu(kAutomationDiagnosticsText), nullptr, nullptr);
      owned_context->automation_probe_text = CreateWindowW(
          L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, hwnd,
          ControlIdToMenu(kAutomationProbeText), nullptr, nullptr);
      owned_context->automation_auto_align_note = CreateWindowW(
          L"STATIC", L"", WS_CHILD, 0, 0, 0, 0, hwnd,
          ControlIdToMenu(kAutomationAutoAlignNote), nullptr, nullptr);

      PopulateBackendCombo(owned_context->capture_backend_combo);
      PopulateBackendCombo(owned_context->render_backend_combo);
      PopulateSourceModeCombo(owned_context->source_mode_combo);
      PopulateSimpleCombo(owned_context->capture_sample_rate_combo,
                          {L"8000", L"16000", L"32000", L"44100", L"48000", L"96000"});
      PopulateSimpleCombo(owned_context->capture_channels_combo,
                          {L"1", L"2", L"4", L"6", L"8"});
      PopulateSimpleCombo(owned_context->capture_sample_type_combo,
                          {L"PCM16", L"PCM24", L"PCM32", L"Float32"});
      PopulateSimpleCombo(owned_context->render_sample_rate_combo,
                          {L"8000", L"16000", L"32000", L"44100", L"48000", L"96000"});
      PopulateSimpleCombo(owned_context->render_channels_combo,
                          {L"1", L"2", L"4", L"6", L"8"});
      PopulateSimpleCombo(owned_context->render_sample_type_combo,
                          {L"PCM16", L"PCM24", L"PCM32", L"Float32"});
      PopulateSimpleCombo(owned_context->capture_share_mode_combo,
                          {L"Shared", L"Exclusive"});
      PopulateSimpleCombo(owned_context->capture_drive_mode_combo,
                          {L"Event", L"Timer"});
      PopulateSimpleCombo(owned_context->render_share_mode_combo,
                          {L"Shared", L"Exclusive"});
      PopulateSimpleCombo(owned_context->render_drive_mode_combo,
                          {L"Event", L"Timer"});
      PopulateSimpleCombo(owned_context->dump_type_combo, {L"WAV", L"PCM"});
      SyncUiFromModel(owned_context);
      owned_context->device_notifications = new DeviceNotificationClient(hwnd);
      owned_context->device_notifications->Register();

      SetTimer(hwnd, kUiTimerId, 33, nullptr);
      SetTimer(hwnd, kAudioTimerId, 10, nullptr);
      return 0;
    }

    case WM_COMMAND: {
      if (context == nullptr) {
        return 0;
      }
      switch (LOWORD(w_param)) {
        case kButtonStart:
          context->model.Start();
          ApplyControlAvailability(context);
          UpdateWindowTitle(hwnd, context);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        case kButtonStop:
          context->model.Stop();
          ApplyControlAvailability(context);
          UpdateWindowTitle(hwnd, context);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        case kButtonRefresh:
          context->model.RefreshDevices(true);
          SyncUiFromModel(context);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        case kButtonProbe:
          if (!context->probe_running) {
            context->probe_mode = ProbeUiMode::Quick;
            SetProbeUiBusy(context, true);
            UpdateWindowTitle(hwnd, context);
            context->probe_thread = std::jthread([hwnd, context]() {
              context->model.RunQuickProbe();
              PostMessageW(hwnd, kMessageProbeFinished, 0, 0);
            });
          }
          return 0;
        case kButtonProbeMatrix:
          if (!context->probe_running) {
            context->probe_mode = ProbeUiMode::Matrix;
            SetProbeUiBusy(context, true);
            UpdateWindowTitle(hwnd, context);
            context->probe_thread = std::jthread([hwnd, context]() {
              context->model.RunProbeMatrix();
              PostMessageW(hwnd, kMessageProbeMatrixFinished, 0, 0);
            });
          }
          return 0;
        case kComboCaptureBackend:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_backend_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureBackend(index == 0 ? AudioBackendType::Wasapi
                                                        : AudioBackendType::WaveApi);
            SyncUiFromModel(context);
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        case kComboRenderBackend:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_backend_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderBackend(index == 0 ? AudioBackendType::Wasapi
                                                       : AudioBackendType::WaveApi);
            SyncUiFromModel(context);
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        case kComboSourceMode:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->source_mode_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureSourceMode(
                index == 0 ? AudioSourceMode::MicrophoneCapture
                           : AudioSourceMode::SystemLoopback);
            SyncUiFromModel(context);
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        case kComboCaptureDevice:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            context->model.SetCaptureDeviceId(
                GetSelectedComboDeviceId(context->capture_device_combo));
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        case kComboRenderDevice:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            context->model.SetRenderDeviceId(
                GetSelectedComboDeviceId(context->render_device_combo));
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        case kEditDelayMs:
          if (HIWORD(w_param) == EN_CHANGE) {
            const auto text = GetWindowTextString(context->delay_edit);
            if (!text.empty()) {
              context->model.SetFixedDelayMs(static_cast<uint32_t>(std::wcstoul(
                  text.c_str(), nullptr, 10)));
            }
          }
          return 0;
        case kCheckboxDump:
          context->model.SetDumpEnabled(
              SendMessageW(context->dump_checkbox, BM_GETCHECK, 0, 0) ==
              BST_CHECKED);
          return 0;
        case kComboCaptureSampleRate:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_sample_rate_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureSampleRate(SampleRateFromComboIndex(index));
          }
          return 0;
        case kComboCaptureChannels:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_channels_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureChannels(ChannelsFromComboIndex(index));
          }
          return 0;
        case kComboCaptureSampleType:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_sample_type_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureSampleType(SampleTypeFromComboIndex(index));
          }
          return 0;
        case kComboRenderSampleRate:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_sample_rate_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderSampleRate(SampleRateFromComboIndex(index));
          }
          return 0;
        case kComboRenderChannels:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_channels_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderChannels(ChannelsFromComboIndex(index));
          }
          return 0;
        case kComboRenderSampleType:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_sample_type_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderSampleType(SampleTypeFromComboIndex(index));
          }
          return 0;
        case kComboCaptureShareMode:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_share_mode_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureWasapiShareMode(
                index == 0 ? WasapiShareMode::Shared
                           : WasapiShareMode::Exclusive);
          }
          return 0;
        case kComboCaptureDriveMode:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_drive_mode_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureWasapiDriveMode(
                index == 0 ? WasapiDriveMode::EventDriven
                           : WasapiDriveMode::TimerDriven);
          }
          return 0;
        case kComboRenderShareMode:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_share_mode_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderWasapiShareMode(
                index == 0 ? WasapiShareMode::Shared
                           : WasapiShareMode::Exclusive);
          }
          return 0;
        case kComboRenderDriveMode:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_drive_mode_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderWasapiDriveMode(
                index == 0 ? WasapiDriveMode::EventDriven
                           : WasapiDriveMode::TimerDriven);
          }
          return 0;
        case kEditDumpPath:
          if (HIWORD(w_param) == EN_CHANGE) {
            context->model.SetDumpPath(
                GetWindowTextString(context->dump_path_edit));
          }
          return 0;
        case kComboDumpType:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->dump_type_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetDumpFileType(index == 0 ? DumpFileType::Wav
                                                      : DumpFileType::RawPcm);
          }
          return 0;
        case kEditCaptureBufferMs:
          if (HIWORD(w_param) == EN_CHANGE) {
            const auto text = GetWindowTextString(context->capture_buffer_edit);
            if (!text.empty()) {
              context->model.SetCaptureBufferDurationMs(static_cast<uint32_t>(
                  std::wcstoul(text.c_str(), nullptr, 10)));
            }
          }
          return 0;
        case kEditRenderBufferMs:
          if (HIWORD(w_param) == EN_CHANGE) {
            const auto text = GetWindowTextString(context->render_buffer_edit);
            if (!text.empty()) {
              context->model.SetRenderBufferDurationMs(static_cast<uint32_t>(
                  std::wcstoul(text.c_str(), nullptr, 10)));
            }
          }
          return 0;
        case kCheckboxMonitor:
          context->model.SetMonitorEnabled(
              SendMessageW(context->monitor_checkbox, BM_GETCHECK, 0, 0) ==
              BST_CHECKED);
          SyncUiFromModel(context);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        case kCheckboxFollowDefaults:
          context->model.SetFollowDefaultDevices(
              SendMessageW(context->follow_defaults_checkbox, BM_GETCHECK, 0, 0) ==
              BST_CHECKED);
          SyncUiFromModel(context);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        case kCheckboxAutoAlignRender:
          context->model.SetAutoAlignRenderFormat(
              SendMessageW(context->auto_align_render_checkbox, BM_GETCHECK, 0, 0) ==
              BST_CHECKED);
          SyncUiFromModel(context);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        default:
          break;
      }
      break;
    }

    case WM_TIMER:
      if (context == nullptr) {
        return 0;
      }
      if (w_param == kAudioTimerId) {
        if (!context->probe_running) {
          context->model.Tick();
        }
        return 0;
      }
      if (w_param == kUiTimerId) {
        ApplyControlAvailability(context);
        SyncAutomationTextMirrors(context);
        UpdateWindowTitle(hwnd, context);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      break;

    case WM_APP_DEVICE_CHANGE:
      if (context != nullptr) {
        context->model.HandleDefaultDeviceRefresh();
        SyncUiFromModel(context);
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;

      case kMessageProbeFinished:
      case kMessageProbeMatrixFinished:
        if (context != nullptr) {
          context->probe_running = false;
          context->probe_mode = ProbeUiMode::None;
          if (!context->shutting_down) {
            SetProbeUiBusy(context, false);
            UpdateWindowTitle(hwnd, context);
            InvalidateRect(hwnd, nullptr, TRUE);
          }
        }
      return 0;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT client_rect {};
      GetClientRect(hwnd, &client_rect);

      if (context != nullptr) {
        RECT summary_rect = client_rect;
        summary_rect.top = 396;
        summary_rect.bottom = 560;
        DrawSummary(hdc, summary_rect, context);

        RECT capability_rect = client_rect;
        capability_rect.top = 566;
        capability_rect.bottom = 670;
        DrawCapabilityPanel(hdc, capability_rect, context);

        RECT probe_rect = client_rect;
        probe_rect.top = 676;
        probe_rect.bottom = 760;
        DrawProbePanel(hdc, probe_rect, context);
        DrawConfigLabels(hdc, context);

        RECT capture_rect = {16, 776, client_rect.right - 16,
                             (client_rect.bottom + 776) / 2 - 8};
        RECT render_rect = {16, capture_rect.bottom + 12, client_rect.right - 16,
                            client_rect.bottom - 16};
        WaveformRenderer::Draw(hdc, capture_rect,
                               context->model.capture_waveform(),
                               RGB(41, 182, 246), L"Capture Waveform",
                               context->model.stats().capture_meter);
        WaveformRenderer::Draw(hdc, render_rect,
                               context->model.render_waveform(),
                               RGB(255, 112, 67), L"Render Waveform",
                               context->model.stats().render_meter);
      }

      EndPaint(hwnd, &ps);
      return 0;
    }

    case WM_DESTROY:
      if (context != nullptr) {
        context->shutting_down = true;
        if (context->probe_thread.joinable()) {
          // Let the active probe own the controller until it exits; stopping the
          // model first can race the background probe against shutdown.
          context->probe_thread.join();
        }
        context->probe_running = false;
        context->probe_mode = ProbeUiMode::None;
        context->model.Stop();
        if (context->device_notifications != nullptr) {
          context->device_notifications->Unregister();
          context->device_notifications->Release();
          context->device_notifications = nullptr;
        }
        ClearDeviceComboHeapStrings(context->capture_device_combo);
        ClearDeviceComboHeapStrings(context->render_device_combo);
      }
      KillTimer(hwnd, kUiTimerId);
      KillTimer(hwnd, kAudioTimerId);
      PostQuitMessage(0);
      return 0;

    default:
      break;
  }

  return DefWindowProcW(hwnd, message, w_param, l_param);
}

}  // namespace

}  // namespace winaudio

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show_command) {
  using namespace winaudio;

  auto context = std::make_unique<WindowContext>();
  if (!context->com.ok()) {
    MessageBoxW(nullptr, L"Failed to initialize COM.", L"WinAudio",
                MB_OK | MB_ICONERROR);
    return 1;
  }

  if (!context->model.Initialize()) {
    MessageBoxW(nullptr, L"Failed to initialize app model.", L"WinAudio",
                MB_OK | MB_ICONERROR);
    return 1;
  }

  WNDCLASSW window_class {};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = instance;
  window_class.lpszClassName = kWindowClassName;
  window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

  RegisterClassW(&window_class);

  HWND hwnd = CreateWindowExW(
      0, kWindowClassName, L"WinAudio Demo", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
      CW_USEDEFAULT, CW_USEDEFAULT, 1100, 820, nullptr, nullptr, instance,
      context.get());

  if (hwnd == nullptr) {
    return 1;
  }

  ShowWindow(hwnd, show_command);

  MSG msg {};
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  context.reset();
  return static_cast<int>(msg.wParam);
}
