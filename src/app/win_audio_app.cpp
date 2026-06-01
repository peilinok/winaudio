#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
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

constexpr int kWindowMinClientWidth = 1120;
constexpr int kWindowMinClientHeight = 1068;
constexpr int kOuterMargin = 16;
constexpr int kSectionGap = 14;
constexpr int kButtonHeight = 30;
constexpr int kButtonGap = 12;
constexpr int kComboHeight = 250;
constexpr int kEditHeight = 24;
constexpr int kSummaryMinHeight = 196;
constexpr int kCapabilityMinHeight = 88;
constexpr int kProbeMinHeight = 104;
constexpr int kWaveformMinHeight = 96;
constexpr int kPanelInset = 16;
constexpr int kPanelGap = 12;
constexpr int kPanelHeaderHeight = 24;

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
constexpr int kEditAppLoopbackProcess = 1125;
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
constexpr int kVisibleSnapshotText = 1950;
constexpr int kVisibleSummaryText = 1951;
constexpr int kVisibleDiagnosticsText = 1952;
constexpr int kVisibleCapabilityText = 1953;
constexpr int kVisibleProbeText = 1954;
constexpr int kVisibleRecentLogsText = 1955;

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
  HWND app_loopback_process_edit = nullptr;
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
  HWND visible_snapshot_text = nullptr;
  HWND visible_summary_text = nullptr;
  HWND visible_diagnostics_text = nullptr;
  HWND visible_capability_text = nullptr;
  HWND visible_probe_text = nullptr;
  HWND visible_recent_logs_text = nullptr;
  HFONT panel_text_font = nullptr;
  HFONT panel_mono_font = nullptr;
  HFONT panel_header_font = nullptr;
  HBRUSH panel_edit_brush = nullptr;
  HBRUSH panel_diagnostic_brush = nullptr;
  std::wstring last_visible_snapshot_text {};
  std::wstring last_visible_summary_text {};
  std::wstring last_visible_diagnostics_text {};
  std::wstring last_visible_capability_text {};
  std::wstring last_visible_probe_text {};
  std::wstring last_visible_recent_logs_text {};
  DeviceNotificationClient* device_notifications = nullptr;
  bool probe_running = false;
  ProbeUiMode probe_mode = ProbeUiMode::None;
  std::jthread probe_thread {};
  bool shutting_down = false;
};

struct AppLayout {
  RECT config_rect {};
  RECT summary_rect {};
  RECT capability_rect {};
  RECT probe_rect {};
  RECT capture_waveform_rect {};
  RECT render_waveform_rect {};
};

struct SummaryPanelLayout {
  RECT snapshot_text_rect {};
  RECT recent_logs_rect {};
  RECT summary_text_rect {};
  RECT diagnostics_text_rect {};
  int summary_right_left = 0;
  int snapshot_title_y = 0;
  int recent_logs_title_y = 0;
  int configured_summary_title_y = 0;
  int diagnostics_title_y = 0;
  bool has_loopback_note = false;
};

struct ConfigPanelLayout {
  RECT toolbar_rect {};
  RECT routing_rect {};
  RECT capture_format_rect {};
  RECT render_format_rect {};
  RECT output_rect {};
  int route_column_width = 0;
  int route_left = 0;
  int route_right = 0;
  int backend_width = 0;
  int source_width = 0;
  int row1_y = 0;
  int row2_y = 0;
  int row1_label_y = 0;
  int row2_label_y = 0;
  int rate_width = 0;
  int channels_width = 0;
  int type_width = 0;
  int mode_width = 0;
  int format_row1_y = 0;
  int format_row2_y = 0;
  int format_row1_label_y = 0;
  int format_row2_label_y = 0;
  int output_left = 0;
  int output_inner_width = 0;
  int output_label_y = 0;
  int output_label_y2 = 0;
  int output_label_y3 = 0;
  int row5_y = 0;
  int row6_y = 0;
  int row7_y = 0;
  int dump_checkbox_width = 0;
  int dump_type_width = 0;
  int dump_path_width = 0;
  int buffer_width = 0;
  int monitor_width = 0;
  int follow_defaults_width = 0;
  int auto_align_width = 0;
};

HMENU ControlIdToMenu(int id) {
  return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

void SetControlText(HWND hwnd, const wchar_t* text) {
  SetWindowTextW(hwnd, text);
}

void SetEditTextIfChanged(HWND hwnd, std::wstring& cache, const std::wstring& text) {
  if (hwnd == nullptr || cache == text) {
    return;
  }
  SetWindowTextW(hwnd, text.c_str());
  cache = text;
}

void ConfigureReadOnlyPanelEdit(HWND hwnd) {
  if (hwnd == nullptr) {
    return;
  }
  constexpr UINT em_setmargins = 0x00D3;
  constexpr WPARAM ec_leftmargin = 0x0001;
  constexpr WPARAM ec_rightmargin = 0x0002;
  const LPARAM margins = MAKELPARAM(8, 8);
  SendMessageW(hwnd, em_setmargins, ec_leftmargin | ec_rightmargin, margins);
}

void UpdateReadOnlyPanelTextRect(HWND hwnd) {
  if (hwnd == nullptr) {
    return;
  }
  constexpr UINT em_setrectnp = 0x00B4;
  RECT client_rect {};
  GetClientRect(hwnd, &client_rect);
  RECT text_rect = client_rect;
  InflateRect(&text_rect, -6, -6);
  SendMessageW(hwnd, em_setrectnp, 0, reinterpret_cast<LPARAM>(&text_rect));
}

HFONT CreatePanelTextFont() {
  NONCLIENTMETRICSW metrics {};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
    LOGFONTW font = metrics.lfMessageFont;
    font.lfHeight -= 1;
    font.lfWeight = FW_NORMAL;
    return CreateFontIndirectW(&font);
  }
  return static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}

HFONT CreatePanelMonoFont() {
  NONCLIENTMETRICSW metrics {};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
    LOGFONTW font = metrics.lfMessageFont;
    font.lfHeight -= 1;
    font.lfWeight = FW_NORMAL;
    wcscpy_s(font.lfFaceName, L"Consolas");
    return CreateFontIndirectW(&font);
  }
  return CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
}

HFONT CreatePanelHeaderFont() {
  NONCLIENTMETRICSW metrics {};
  metrics.cbSize = sizeof(metrics);
  if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
    LOGFONTW font = metrics.lfMessageFont;
    font.lfHeight += 2;
    font.lfWeight = FW_SEMIBOLD;
    return CreateFontIndirectW(&font);
  }
  return CreateFontW(-18, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void ApplyPanelTextFont(HWND hwnd, HFONT font) {
  if (hwnd == nullptr || font == nullptr) {
    return;
  }
  SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void SetControlVisible(HWND hwnd, bool visible) {
  if (hwnd == nullptr) {
    return;
  }
  ShowWindow(hwnd, visible ? SW_SHOW : SW_HIDE);
}

int RectWidth(const RECT& rect) {
  return rect.right - rect.left;
}

int RectHeight(const RECT& rect) {
  return rect.bottom - rect.top;
}

RECT MakeRect(int left, int top, int width, int height) {
  return RECT {left, top, left + width, top + height};
}

void MoveControl(HWND hwnd, const RECT& rect) {
  if (hwnd == nullptr) {
    return;
  }
  MoveWindow(hwnd, rect.left, rect.top, RectWidth(rect), RectHeight(rect), TRUE);
}

void FillPanelRect(HDC hdc, const RECT& rect, COLORREF fill, COLORREF border) {
  HBRUSH fill_brush = CreateSolidBrush(fill);
  FillRect(hdc, &rect, fill_brush);
  DeleteObject(fill_brush);

  HBRUSH border_brush = CreateSolidBrush(border);
  FrameRect(hdc, &rect, border_brush);
  DeleteObject(border_brush);
}

AppLayout CalculateAppLayout(const RECT& client_rect) {
  AppLayout layout {};
  const int client_width = std::max(RectWidth(client_rect), kWindowMinClientWidth);
  const int client_height =
      std::max(RectHeight(client_rect), kWindowMinClientHeight);
  const int inner_width = client_width - (kOuterMargin * 2);

  const int config_height = 398;
  const int vertical_budget =
      client_height - (kOuterMargin * 2) - config_height - (kSectionGap * 4);
  const int base_middle_height =
      kSummaryMinHeight + kCapabilityMinHeight + kProbeMinHeight +
      (kWaveformMinHeight * 2);
  const int extra_height = std::max(0, vertical_budget - base_middle_height);
  const int summary_height = kSummaryMinHeight + ((extra_height * 9) / 20);
  const int capability_height = kCapabilityMinHeight + (extra_height / 10);
  const int probe_height = kProbeMinHeight + ((extra_height * 3) / 20);
  const int waveform_extra =
      std::max(0, vertical_budget - summary_height - capability_height - probe_height -
                      (kWaveformMinHeight * 2));
  const int capture_waveform_height = kWaveformMinHeight + (waveform_extra / 2);
  const int render_waveform_height =
      kWaveformMinHeight + (waveform_extra - (waveform_extra / 2));

  layout.config_rect = MakeRect(kOuterMargin, kOuterMargin, inner_width, config_height);
  int top = layout.config_rect.bottom + kSectionGap;
  layout.summary_rect = MakeRect(kOuterMargin, top, inner_width, summary_height);
  top = layout.summary_rect.bottom + kSectionGap;
  layout.capability_rect =
      MakeRect(kOuterMargin, top, inner_width, capability_height);
  top = layout.capability_rect.bottom + kSectionGap;
  layout.probe_rect = MakeRect(kOuterMargin, top, inner_width, probe_height);
  top = layout.probe_rect.bottom + kSectionGap;

  layout.capture_waveform_rect =
      MakeRect(kOuterMargin, top, inner_width, capture_waveform_height);
  top = layout.capture_waveform_rect.bottom + kSectionGap;
  layout.render_waveform_rect =
      MakeRect(kOuterMargin, top, inner_width, render_waveform_height);

  return layout;
}

SummaryPanelLayout CalculateSummaryPanelLayout(const RECT& rect,
                                               AudioSourceMode source_mode) {
  SummaryPanelLayout layout {};
  const int summary_inner_width = RectWidth(rect) - (kPanelInset * 2);
  const int summary_left_width =
      std::clamp((summary_inner_width * 33) / 100, 280, 360);
  const int summary_right_width =
      summary_inner_width - summary_left_width - kPanelGap;
  layout.summary_right_left =
      rect.left + kPanelInset + summary_left_width + kPanelGap;

  const auto loopback_note = BuildLoopbackCaptureNoteText(source_mode);
  layout.has_loopback_note = !loopback_note.empty();
  layout.snapshot_title_y = rect.top + 12;
  const int left_content_top = rect.top + 50;
  const int left_bottom = static_cast<int>(rect.bottom) - kPanelInset;
  const int left_content_height = std::max(132, left_bottom - left_content_top);
  const int desired_snapshot_height = layout.has_loopback_note ? 94 : 76;
  const int snapshot_height =
      std::clamp(desired_snapshot_height, 68, std::max(68, left_content_height - 72));
  layout.snapshot_text_rect =
      MakeRect(rect.left + kPanelInset, left_content_top, summary_left_width,
               snapshot_height);
  layout.recent_logs_title_y = layout.snapshot_text_rect.bottom + 8;
  const int logs_top = layout.recent_logs_title_y + 18;
  const int logs_height =
      std::max(36, static_cast<int>(rect.bottom) - logs_top - kPanelInset);
  layout.recent_logs_rect =
      MakeRect(rect.left + kPanelInset, logs_top, summary_left_width, logs_height);

  layout.configured_summary_title_y = rect.top + 12;
  const int right_content_top = rect.top + 50;
  const int right_bottom = static_cast<int>(rect.bottom) - kPanelInset;
  const int right_content_height = std::max(96, right_bottom - right_content_top);
  const int summary_text_height = std::clamp((right_content_height * 48) / 100, 64,
                                             std::max(64, right_content_height - 64));
  layout.summary_text_rect =
      MakeRect(layout.summary_right_left, right_content_top, summary_right_width,
               summary_text_height);
  const int diagnostics_anchor_y =
      static_cast<int>(layout.summary_text_rect.bottom) + 10;
  layout.diagnostics_title_y =
      std::min(diagnostics_anchor_y, right_bottom - 44);
  const int diagnostics_top =
      std::min(layout.diagnostics_title_y + 18, right_bottom - 24);
  layout.diagnostics_text_rect =
      MakeRect(layout.summary_right_left, diagnostics_top, summary_right_width,
               std::max(24, right_bottom - diagnostics_top));

  return layout;
}

std::wstring BuildSessionSnapshotText(WindowContext* context) {
  if (context == nullptr) {
    return {};
  }

  const auto stats = context->model.stats();
  const auto devices = context->model.devices();
  const auto source_mode = context->model.configuration().capture.source_mode;

  std::wstring text = L"State: " + context->model.session_state();
  text += L"\r\n";
  text += BuildDeviceCountLineText(source_mode, devices.capture_devices.size(),
                                   devices.render_devices.size());

  const auto loopback_note = BuildLoopbackCaptureNoteText(source_mode);
  if (!loopback_note.empty()) {
    text += L"\r\n";
    text += loopback_note;
  }

  text += L"\r\nQueue: " + std::to_wstring(stats.queue_depth_ms) +
          L" ms | Delay estimate: " +
          std::to_wstring(stats.estimated_monitor_delay_ms) + L" ms";
  text += L"\r\nDropped: " + std::to_wstring(stats.dropped_frames) +
          L" | Underruns: " + std::to_wstring(stats.render_underruns);
  return text;
}

std::vector<std::wstring> SplitDisplayLines(const std::wstring& text) {
  std::vector<std::wstring> lines;
  std::wstring current;
  for (size_t index = 0; index < text.size(); ++index) {
    const wchar_t ch = text[index];
    if (ch == L'\r') {
      if (index + 1 < text.size() && text[index + 1] == L'\n') {
        ++index;
      }
      lines.push_back(current);
      current.clear();
      continue;
    }
    if (ch == L'\n') {
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  lines.push_back(current);
  return lines;
}

void AppendDisplaySection(std::wstring& text,
                          const wchar_t* title,
                          const std::vector<std::wstring>& lines) {
  if (lines.empty()) {
    return;
  }
  if (!text.empty()) {
    text += L"\r\n\r\n";
  }
  text += L"[";
  text += title;
  text += L"]";
  for (const auto& line : lines) {
    if (line.empty()) {
      continue;
    }
    text += L"\r\n";
    text += line;
  }
}

std::wstring FormatSummaryPanelText(const std::wstring& text) {
  if (text.empty()) {
    return text;
  }

  std::vector<std::wstring> configured;
  std::vector<std::wstring> flags;
  std::vector<std::wstring> timing;
  std::vector<std::wstring> routing;
  std::vector<std::wstring> notes;

  for (const auto& line : SplitDisplayLines(text)) {
    if (line.empty()) {
      continue;
    }
    if (line.rfind(L"Capture:", 0) == 0 || line.rfind(L"Render:", 0) == 0 ||
        line.rfind(L"Capture WASAPI:", 0) == 0 ||
        line.rfind(L"Render WASAPI:", 0) == 0) {
      configured.push_back(line);
    } else if (line.rfind(L"Follow defaults:", 0) == 0 ||
               line.rfind(L"Render auto-align:", 0) == 0 ||
               line.rfind(L"Monitor:", 0) == 0 ||
               line.rfind(L"Dump:", 0) == 0) {
      flags.push_back(line);
    } else if (line.rfind(L"Monitor delay:", 0) == 0 ||
               line.rfind(L"Capture buffer:", 0) == 0 ||
               line.rfind(L"Render buffer:", 0) == 0 ||
               line.rfind(L"Effective render request:", 0) == 0) {
      timing.push_back(line);
    } else if (line.rfind(L"Device selection follows", 0) == 0 ||
               line.rfind(L"App loopback target:", 0) == 0) {
      routing.push_back(line);
    } else {
      notes.push_back(line);
    }
  }

  std::wstring formatted;
  AppendDisplaySection(formatted, L"Configured", configured);
  AppendDisplaySection(formatted, L"Session Flags", flags);
  AppendDisplaySection(formatted, L"Timing", timing);
  AppendDisplaySection(formatted, L"Routing", routing);
  AppendDisplaySection(formatted, L"Notes", notes);
  return formatted;
}

std::wstring FormatDiagnosticsPanelText(const std::wstring& text) {
  if (text.empty()) {
    return text;
  }

  std::vector<std::wstring> configured;
  std::vector<std::wstring> requested;
  std::vector<std::wstring> negotiated;
  std::vector<std::wstring> runtime;
  std::vector<std::wstring> timing;
  std::vector<std::wstring> tracking;
  std::vector<std::wstring> selection;
  std::vector<std::wstring> issues;

  for (const auto& line : SplitDisplayLines(text)) {
    if (line.empty()) {
      continue;
    }
    if (line.rfind(L"Current configured", 0) == 0 ||
        line.rfind(L"Effective configured render request:", 0) == 0) {
      configured.push_back(line);
    } else if (line.rfind(L"Active session requested", 0) == 0 ||
               line.rfind(L"Active capture WASAPI request:", 0) == 0 ||
               line.rfind(L"Active render WASAPI request:", 0) == 0) {
      requested.push_back(line);
    } else if (line.rfind(L"Active session negotiated", 0) == 0) {
      negotiated.push_back(line);
    } else if (line.rfind(L"Active capture mode:", 0) == 0 ||
               line.rfind(L"Active render mode:", 0) == 0 ||
               line.rfind(L"Active resampler:", 0) == 0 ||
               line.rfind(L"Active capture runtime:", 0) == 0 ||
               line.rfind(L"Active render runtime:", 0) == 0) {
      runtime.push_back(line);
    } else if (line.rfind(L"Active monitor delay:", 0) == 0 ||
               line.rfind(L"Active capture buffer:", 0) == 0 ||
               line.rfind(L"Active render buffer:", 0) == 0 ||
               line.rfind(L"Active dump path:", 0) == 0) {
      timing.push_back(line);
    } else if (line.rfind(L"Last ", 0) == 0 ||
               line.rfind(L"Running session note:", 0) == 0 ||
               line.rfind(L"Device tracking:", 0) == 0 ||
               line.rfind(L"Application loopback target process:", 0) == 0 ||
               line.rfind(L"Render monitor playback is disabled", 0) == 0) {
      tracking.push_back(line);
    } else if (line.rfind(L"Selected ", 0) == 0) {
      selection.push_back(line);
    } else {
      issues.push_back(line);
    }
  }

  std::wstring formatted;
  AppendDisplaySection(formatted, L"Configured", configured);
  AppendDisplaySection(formatted, L"Requested", requested);
  AppendDisplaySection(formatted, L"Negotiated", negotiated);
  AppendDisplaySection(formatted, L"Runtime", runtime);
  AppendDisplaySection(formatted, L"Timing / Dump", timing);
  AppendDisplaySection(formatted, L"Tracking", tracking);
  AppendDisplaySection(formatted, L"Selection", selection);
  AppendDisplaySection(formatted, L"Other", issues);
  return formatted;
}

std::wstring FormatCapabilityPanelText(const std::wstring& text) {
  if (text.empty()) {
    return text;
  }

  std::wstring formatted = text;
  auto insert_gap = [&](const std::wstring& marker) {
    const std::wstring replacement = L"\r\n\r\n" + marker;
    size_t position = 0;
    while ((position = formatted.find(marker, position)) != std::wstring::npos) {
      if (position == 0 || formatted.rfind(L"\r\n\r\n", position) == position - 4) {
        position += marker.size();
        continue;
      }
      formatted.replace(position, marker.size(), replacement);
      position += replacement.size();
    }
  };

  insert_gap(L"Current Limitations");
  insert_gap(L"Current Strategy");
  insert_gap(L"Support Probes");
  insert_gap(L"- Capture probe ");
  insert_gap(L"- Render probe ");
  return formatted;
}

std::wstring FormatProbePanelText(const std::wstring& text) {
  if (text.empty()) {
    return text;
  }

  std::wstring formatted = text;
  const std::vector<std::wstring> markers = {
      L"QuickSummary:",
      L"Stage:",
      L"RequestedCapture:",
      L"RequestedRender:",
      L"RequestedCaptureDeviceId:",
      L"RequestedRenderDeviceId:",
      L"FailureStage:",
      L"Result:",
      L"MatrixSummary:",
      L"MatrixHint:",
      L"BackendSummary:",
      L"PairSummary:",
      L"ProfileSummary:",
      L"SourceSummary:",
      L"AlignSummary:",
      L"DelaySummary:",
      L"BufferSummary:",
      L"Last probe matrix"};

  for (const auto& marker : markers) {
    const std::wstring replacement = L"\r\n\r\n" + marker;
    size_t position = 0;
    while ((position = formatted.find(marker, position)) != std::wstring::npos) {
      if (position == 0 || formatted.rfind(L"\r\n\r\n", position) == position - 4) {
        position += marker.size();
        continue;
      }
      formatted.replace(position, marker.size(), replacement);
      position += replacement.size();
    }
  }

  const std::vector<std::wstring> list_markers = {
      L"- BackendSummary:",
      L"- PairSummary:",
      L"- ProfileSummary:",
      L"- SourceSummary:",
      L"- AlignSummary:",
      L"- DelaySummary:",
      L"- BufferSummary:"};
  for (const auto& marker : list_markers) {
    const std::wstring replacement = L"\r\n" + marker;
    size_t position = 0;
    while ((position = formatted.find(marker, position)) != std::wstring::npos) {
      if (position == 0 || formatted.rfind(L"\r\n", position) == position - 2) {
        position += marker.size();
        continue;
      }
      formatted.replace(position, marker.size(), replacement);
      position += replacement.size();
    }
  }

  const std::vector<std::wstring> primary_markers = {
      L"MatrixSummary:",
      L"MatrixHint:",
      L"BackendSummary:",
      L"PairSummary:",
      L"ProfileSummary:",
      L"SourceSummary:",
      L"AlignSummary:",
      L"DelaySummary:",
      L"BufferSummary:"};
  for (const auto& marker : primary_markers) {
    const std::wstring replacement = L"\r\n\r\n[" + marker + L"] ";
    size_t position = 0;
    while ((position = formatted.find(marker, position)) != std::wstring::npos) {
      const bool at_line_start =
          position == 0 || formatted.rfind(L"\r\n", position) == position - 2;
      if (!at_line_start) {
        position += marker.size();
        continue;
      }
      formatted.replace(position, marker.size(), L"[" + marker + L"]");
      position += marker.size() + 2;
    }
  }

  return formatted;
}

ConfigPanelLayout CalculateConfigPanelLayout(const RECT& rect) {
  ConfigPanelLayout layout {};
  const int section_inset = 16;
  const int inner_gap = 12;
  const int content_width = RectWidth(rect);
  const int format_gap = 12;
  const int format_width = (content_width - format_gap) / 2;

  layout.toolbar_rect = MakeRect(rect.left, rect.top, content_width, 42);
  layout.routing_rect = MakeRect(rect.left, rect.top + 34, content_width, 126);
  layout.capture_format_rect =
      MakeRect(rect.left, rect.top + 168, format_width, 108);
  layout.render_format_rect =
      MakeRect(rect.left + format_width + format_gap, rect.top + 168,
               content_width - format_width - format_gap, 108);
  layout.output_rect =
      MakeRect(rect.left, rect.top + 284, content_width,
               rect.bottom - (rect.top + 284));

  layout.route_column_width =
      (RectWidth(layout.routing_rect) - (section_inset * 2) - inner_gap) / 2;
  layout.route_left = layout.routing_rect.left + section_inset;
  layout.route_right = layout.route_left + layout.route_column_width + inner_gap;
  layout.backend_width = 180;
  layout.source_width = layout.route_column_width - layout.backend_width - inner_gap;
  layout.row1_y = layout.routing_rect.top + 38;
  layout.row2_y = layout.row1_y + 64;
  layout.row1_label_y = layout.routing_rect.top + 20;
  layout.row2_label_y = layout.row1_label_y + 64;

  layout.rate_width = 110;
  layout.channels_width = 82;
  layout.type_width = RectWidth(layout.capture_format_rect) - (section_inset * 2) -
                      layout.rate_width - layout.channels_width - (inner_gap * 2);
  layout.mode_width =
      (RectWidth(layout.capture_format_rect) - (section_inset * 2) - inner_gap) / 2;
  layout.format_row1_y = layout.capture_format_rect.top + 34;
  layout.format_row2_y = layout.format_row1_y + 34;
  layout.format_row1_label_y = layout.capture_format_rect.top + 16;
  layout.format_row2_label_y = layout.format_row1_label_y + 34;

  layout.output_left = layout.output_rect.left + section_inset;
  layout.output_inner_width = RectWidth(layout.output_rect) - (section_inset * 2);
  layout.output_label_y = layout.output_rect.top + 20;
  layout.output_label_y2 = layout.output_label_y + 30;
  layout.output_label_y3 = layout.output_label_y2 + 30;
  layout.row5_y = layout.output_rect.top + 28;
  layout.row6_y = layout.row5_y + 30;
  layout.row7_y = layout.row6_y + 30;
  layout.dump_checkbox_width = 146;
  layout.dump_type_width = 160;
  layout.dump_path_width =
      layout.output_inner_width - layout.dump_checkbox_width -
      layout.dump_type_width - (inner_gap * 2);
  layout.buffer_width = 140;
  layout.monitor_width = 168;
  layout.follow_defaults_width = 208;
  layout.auto_align_width = 180;

  return layout;
}

void LayoutChildControls(HWND hwnd, WindowContext* context) {
  if (context == nullptr) {
    return;
  }

  RECT client_rect {};
  GetClientRect(hwnd, &client_rect);
  const AppLayout layout = CalculateAppLayout(client_rect);
  const auto source_mode = context->model.configuration().capture.source_mode;
  const auto config_layout = CalculateConfigPanelLayout(layout.config_rect);
  const int inner_gap = 12;

  const int button_y = config_layout.toolbar_rect.top + 8;
  int x = config_layout.toolbar_rect.left + 8;
  MoveControl(context->start_button, MakeRect(x, button_y, 128, kButtonHeight));
  x += 128 + kButtonGap;
  MoveControl(context->stop_button, MakeRect(x, button_y, 92, kButtonHeight));
  x += 92 + kButtonGap;
  MoveControl(context->refresh_button, MakeRect(x, button_y, 140, kButtonHeight));
  x += 140 + kButtonGap;
  MoveControl(context->probe_button, MakeRect(x, button_y, 150, kButtonHeight));
  x += 150 + kButtonGap;
  MoveControl(context->probe_matrix_button,
              MakeRect(x, button_y, 162, kButtonHeight));

  x = config_layout.route_left;
  MoveControl(context->capture_backend_combo,
              MakeRect(x, config_layout.row1_y, config_layout.backend_width,
                       kComboHeight));
  x += config_layout.backend_width + inner_gap;
  MoveControl(context->source_mode_combo,
              MakeRect(x, config_layout.row1_y, config_layout.source_width,
                       kComboHeight));

  x = config_layout.route_right;
  MoveControl(context->render_backend_combo,
              MakeRect(x, config_layout.row1_y, config_layout.backend_width,
                       kComboHeight));
  x += config_layout.backend_width + inner_gap;
  MoveControl(context->delay_edit,
              MakeRect(x, config_layout.row1_y, 118, kEditHeight));

  MoveControl(context->capture_device_combo,
              MakeRect(config_layout.route_left, config_layout.row2_y,
                       config_layout.route_column_width, kComboHeight));
  MoveControl(context->render_device_combo,
              MakeRect(config_layout.route_right, config_layout.row2_y,
                       config_layout.route_column_width, kComboHeight));
  MoveControl(context->app_loopback_process_edit,
              MakeRect(config_layout.route_left, config_layout.row2_y,
                       config_layout.route_column_width, kEditHeight));

  x = config_layout.capture_format_rect.left + 16;
  MoveControl(context->capture_sample_rate_combo,
              MakeRect(x, config_layout.format_row1_y, config_layout.rate_width,
                       kComboHeight));
  x += config_layout.rate_width + inner_gap;
  MoveControl(context->capture_channels_combo,
              MakeRect(x, config_layout.format_row1_y, config_layout.channels_width,
                       kComboHeight));
  x += config_layout.channels_width + inner_gap;
  MoveControl(context->capture_sample_type_combo,
              MakeRect(x, config_layout.format_row1_y, config_layout.type_width,
                       kComboHeight));
  x = config_layout.capture_format_rect.left + 16;
  MoveControl(context->capture_share_mode_combo,
              MakeRect(x, config_layout.format_row2_y, config_layout.mode_width,
                       kComboHeight));
  x += config_layout.mode_width + inner_gap;
  MoveControl(context->capture_drive_mode_combo,
              MakeRect(x, config_layout.format_row2_y, config_layout.mode_width,
                       kComboHeight));

  x = config_layout.render_format_rect.left + 16;
  MoveControl(context->render_sample_rate_combo,
              MakeRect(x, config_layout.format_row1_y, config_layout.rate_width,
                       kComboHeight));
  x += config_layout.rate_width + inner_gap;
  MoveControl(context->render_channels_combo,
              MakeRect(x, config_layout.format_row1_y, config_layout.channels_width,
                       kComboHeight));
  x += config_layout.channels_width + inner_gap;
  MoveControl(context->render_sample_type_combo,
              MakeRect(x, config_layout.format_row1_y, config_layout.type_width,
                       kComboHeight));
  x = config_layout.render_format_rect.left + 16;
  MoveControl(context->render_share_mode_combo,
              MakeRect(x, config_layout.format_row2_y, config_layout.mode_width,
                       kComboHeight));
  x += config_layout.mode_width + inner_gap;
  MoveControl(context->render_drive_mode_combo,
              MakeRect(x, config_layout.format_row2_y, config_layout.mode_width,
                       kComboHeight));

  MoveControl(context->dump_path_edit,
              MakeRect(config_layout.output_left, config_layout.row5_y,
                       config_layout.dump_path_width, kEditHeight));
  MoveControl(context->dump_checkbox,
              MakeRect(config_layout.output_left + config_layout.dump_path_width +
                           inner_gap,
                       config_layout.row5_y, config_layout.dump_checkbox_width,
                       kEditHeight));
  MoveControl(context->dump_type_combo,
              MakeRect(config_layout.output_left + config_layout.dump_path_width +
                           config_layout.dump_checkbox_width +
                           (inner_gap * 2),
                       config_layout.row5_y, config_layout.dump_type_width,
                       kComboHeight));

  MoveControl(context->capture_buffer_edit,
              MakeRect(config_layout.output_left, config_layout.row6_y,
                       config_layout.buffer_width, kEditHeight));
  MoveControl(context->render_buffer_edit,
              MakeRect(config_layout.output_left + config_layout.buffer_width + inner_gap,
                       config_layout.row6_y, config_layout.buffer_width, kEditHeight));
  MoveControl(context->monitor_checkbox,
              MakeRect(config_layout.output_left, config_layout.row7_y,
                       config_layout.monitor_width,
                       kEditHeight));
  MoveControl(context->follow_defaults_checkbox,
              MakeRect(config_layout.output_left + config_layout.monitor_width + inner_gap,
                       config_layout.row7_y, config_layout.follow_defaults_width,
                       kEditHeight));
  MoveControl(context->auto_align_render_checkbox,
              MakeRect(config_layout.output_left + config_layout.monitor_width +
                           config_layout.follow_defaults_width + (inner_gap * 2),
                       config_layout.row7_y, config_layout.auto_align_width,
                       kEditHeight));

  const RECT& summary = layout.summary_rect;
  const RECT& capability = layout.capability_rect;
  const RECT& probe = layout.probe_rect;
  const auto summary_layout = CalculateSummaryPanelLayout(summary, source_mode);
  MoveControl(context->visible_snapshot_text,
              summary_layout.snapshot_text_rect);
  MoveControl(context->visible_summary_text,
              summary_layout.summary_text_rect);
  MoveControl(context->visible_diagnostics_text,
              summary_layout.diagnostics_text_rect);
  MoveControl(context->visible_recent_logs_text,
              summary_layout.recent_logs_rect);
  MoveControl(context->visible_capability_text,
              MakeRect(capability.left + kPanelInset,
                       capability.top + kPanelInset + 32,
                       RectWidth(capability) - (kPanelInset * 2),
                       RectHeight(capability) - ((kPanelInset * 2) + 32)));
  MoveControl(context->visible_probe_text,
              MakeRect(probe.left + kPanelInset,
                       probe.top + kPanelInset + 32,
                       RectWidth(probe) - (kPanelInset * 2),
                       RectHeight(probe) - ((kPanelInset * 2) + 32)));
  UpdateReadOnlyPanelTextRect(context->visible_snapshot_text);
  UpdateReadOnlyPanelTextRect(context->visible_recent_logs_text);
  UpdateReadOnlyPanelTextRect(context->visible_summary_text);
  UpdateReadOnlyPanelTextRect(context->visible_diagnostics_text);
  UpdateReadOnlyPanelTextRect(context->visible_capability_text);
  UpdateReadOnlyPanelTextRect(context->visible_probe_text);
}

void ApplyControlAvailability(WindowContext* context) {
  const auto config = context->model.configuration();
  const auto session_state = context->model.session_state();
  const bool busy = context->probe_running;
  const bool monitor_enabled = config.render.monitor_enabled;
  const bool application_loopback =
      config.capture.source_mode == AudioSourceMode::ApplicationLoopback;
  const BOOL general_enabled = busy ? FALSE : TRUE;
  const BOOL capture_device_combo_enabled =
      (!busy && !config.follow_default_devices && !application_loopback) ? TRUE
                                                                         : FALSE;
  const BOOL application_loopback_target_enabled =
      (!busy && application_loopback) ? TRUE : FALSE;
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
      context->app_loopback_process_edit,
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
  if (context->app_loopback_process_edit != nullptr) {
    EnableWindow(context->app_loopback_process_edit,
                 application_loopback_target_enabled);
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
  const auto source_mode = context->model.configuration().capture.source_mode;

  if (context->automation_capture_label != nullptr) {
    SetWindowTextW(context->automation_capture_label,
                   BuildCaptureDeviceLabelText(source_mode).c_str());
  }
  if (context->automation_device_count_line != nullptr) {
    SetWindowTextW(
        context->automation_device_count_line,
        BuildDeviceCountLineText(source_mode, devices.capture_devices.size(),
                                 devices.render_devices.size())
            .c_str());
  }
  if (context->automation_summary_text != nullptr) {
    SetWindowTextW(context->automation_summary_text,
                   context->model.summary_text().c_str());
  }
  if (context->visible_snapshot_text != nullptr) {
    SetEditTextIfChanged(context->visible_snapshot_text,
                         context->last_visible_snapshot_text,
                         BuildSessionSnapshotText(context));
  }
  if (context->visible_summary_text != nullptr) {
    SetEditTextIfChanged(context->visible_summary_text,
                         context->last_visible_summary_text,
                         FormatSummaryPanelText(context->model.summary_text()));
  }
  if (context->automation_diagnostics_text != nullptr) {
    SetWindowTextW(context->automation_diagnostics_text,
                   context->model.diagnostics_text().c_str());
  }
  if (context->visible_diagnostics_text != nullptr) {
    SetEditTextIfChanged(context->visible_diagnostics_text,
                         context->last_visible_diagnostics_text,
                         FormatDiagnosticsPanelText(context->model.diagnostics_text()));
  }
  if (context->automation_probe_text != nullptr) {
    SetWindowTextW(context->automation_probe_text,
                   context->model.probe_text().c_str());
  }
  if (context->visible_probe_text != nullptr) {
    SetEditTextIfChanged(context->visible_probe_text,
                         context->last_visible_probe_text,
                         FormatProbePanelText(context->model.probe_text()));
  }
  if (context->visible_recent_logs_text != nullptr) {
    std::wstring log_text;
    const auto& logs = context->model.logs();
    if (logs.empty()) {
      log_text = L"No recent logs yet.";
    } else {
      for (auto it = logs.rbegin(); it != logs.rend(); ++it) {
        if (!log_text.empty()) {
          log_text += L"\r\n";
        }
        log_text += *it;
      }
    }
    SetEditTextIfChanged(context->visible_recent_logs_text,
                         context->last_visible_recent_logs_text, log_text);
  }
  if (context->automation_auto_align_note != nullptr) {
    SetWindowTextW(context->automation_auto_align_note,
                   BuildAutoAlignExplanatoryNoteText().c_str());
  }
  if (context->visible_capability_text != nullptr) {
    SetEditTextIfChanged(context->visible_capability_text,
                         context->last_visible_capability_text,
                         FormatCapabilityPanelText(context->model.capability_text()));
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
  SendMessageW(combo, CB_ADDSTRING, 0,
               reinterpret_cast<LPARAM>(L"Application Loopback"));
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
  const bool application_loopback =
      config.capture.source_mode == AudioSourceMode::ApplicationLoopback;

  SendMessageW(context->capture_backend_combo, CB_SETCURSEL,
               config.capture.backend == AudioBackendType::Wasapi ? 0 : 1, 0);
  SendMessageW(context->render_backend_combo, CB_SETCURSEL,
               config.render.backend == AudioBackendType::Wasapi ? 0 : 1, 0);
  SendMessageW(context->source_mode_combo, CB_SETCURSEL,
               config.capture.source_mode == AudioSourceMode::MicrophoneCapture
                   ? 0
                   : (config.capture.source_mode == AudioSourceMode::SystemLoopback
                          ? 1
                          : 2),
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
  SetControlText(context->app_loopback_process_edit,
                 config.capture.application_loopback_process.c_str());
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
  SetControlVisible(context->capture_device_combo, !application_loopback);
  SetControlVisible(context->app_loopback_process_edit, application_loopback);
  ApplyControlAvailability(context);
  SyncAutomationTextMirrors(context);
}

void DrawConfigLabels(HDC hdc, const RECT& rect, AudioSourceMode source_mode) {
  const auto layout = CalculateConfigPanelLayout(rect);
  const int section_inset = 16;
  const int inner_gap = 12;

  FillPanelRect(hdc, layout.toolbar_rect, RGB(240, 245, 250), RGB(214, 223, 232));
  FillPanelRect(hdc, layout.routing_rect, RGB(248, 250, 252), RGB(224, 229, 235));
  FillPanelRect(hdc, layout.capture_format_rect, RGB(247, 249, 252),
                RGB(223, 228, 234));
  FillPanelRect(hdc, layout.render_format_rect, RGB(247, 249, 252),
                RGB(223, 228, 234));
  FillPanelRect(hdc, layout.output_rect, RGB(246, 249, 251), RGB(220, 226, 232));

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(45, 50, 56));
  TextOutW(hdc, rect.left + 16, rect.top + 12, L"Session Controls", 16);
  TextOutW(hdc, rect.left + 16, rect.top + 50, L"Signal Routing", 14);
  TextOutW(hdc, layout.capture_format_rect.left + 16,
           layout.capture_format_rect.top + 10,
           L"Capture Format", 14);
  TextOutW(hdc, layout.render_format_rect.left + 16,
           layout.render_format_rect.top + 10,
           L"Render Format", 13);
  TextOutW(hdc, layout.output_rect.left + 16, layout.output_rect.top + 10,
           L"Session Output", 14);
  TextOutW(hdc, layout.route_left, layout.row1_label_y, L"Capture Backend", 15);
  TextOutW(hdc, layout.route_left + layout.backend_width + inner_gap,
           layout.row1_label_y, L"Source Mode", 11);
  TextOutW(hdc, layout.route_right, layout.row1_label_y, L"Render Backend", 14);
  TextOutW(hdc, layout.route_right + layout.backend_width + inner_gap,
           layout.row1_label_y, L"Delay (ms)", 10);

  const bool application_loopback =
      source_mode == AudioSourceMode::ApplicationLoopback;
  const auto capture_device_label = application_loopback
                                        ? std::wstring {L"Target Process / PID"}
                                        : BuildCaptureDeviceLabelText(source_mode);
  TextOutW(hdc, layout.route_left, layout.row2_label_y, capture_device_label.c_str(),
           static_cast<int>(capture_device_label.size()));
  TextOutW(hdc, layout.route_right, layout.row2_label_y, L"Render Device", 13);

  TextOutW(hdc, layout.capture_format_rect.left + section_inset,
           layout.format_row1_label_y,
           L"Rate", 4);
  TextOutW(hdc,
           layout.capture_format_rect.left + section_inset + layout.rate_width +
               inner_gap,
           layout.format_row1_label_y, L"Ch", 2);
  TextOutW(hdc,
           layout.capture_format_rect.left + section_inset + layout.rate_width +
               layout.channels_width + (inner_gap * 2),
           layout.format_row1_label_y, L"Type", 4);
  TextOutW(hdc, layout.capture_format_rect.left + section_inset,
           layout.format_row2_label_y,
           L"Share", 5);
  TextOutW(hdc,
           layout.capture_format_rect.left + section_inset + layout.mode_width +
               inner_gap,
           layout.format_row2_label_y, L"Drive", 5);

  TextOutW(hdc, layout.render_format_rect.left + section_inset,
           layout.format_row1_label_y,
           L"Rate", 4);
  TextOutW(hdc,
           layout.render_format_rect.left + section_inset + layout.rate_width +
               inner_gap,
           layout.format_row1_label_y, L"Ch", 2);
  TextOutW(hdc,
           layout.render_format_rect.left + section_inset + layout.rate_width +
               layout.channels_width + (inner_gap * 2),
           layout.format_row1_label_y, L"Type", 4);
  TextOutW(hdc, layout.render_format_rect.left + section_inset,
           layout.format_row2_label_y,
           L"Share", 5);
  TextOutW(hdc,
           layout.render_format_rect.left + section_inset + layout.mode_width +
               inner_gap,
           layout.format_row2_label_y, L"Drive", 5);

  TextOutW(hdc, layout.output_rect.left + section_inset, layout.output_label_y,
           L"Dump Path", 9);
  TextOutW(hdc, layout.output_rect.right - 160, layout.output_label_y,
           L"Dump Type", 9);
  TextOutW(hdc, layout.output_rect.left + section_inset, layout.output_label_y2,
           L"Cap Buffer (ms)", 15);
  TextOutW(hdc,
           layout.output_rect.left + section_inset + layout.buffer_width + 12,
           layout.output_label_y2, L"Ren Buffer (ms)", 15);
  TextOutW(hdc, layout.output_rect.left + section_inset, layout.output_label_y3,
           L"Session Flags", 13);
}

void DrawConfigLabels(HDC hdc, WindowContext* context) {
  if (context == nullptr) {
    return;
  }
  RECT client_rect {};
  GetClientRect(WindowFromDC(hdc), &client_rect);
  const AppLayout layout = CalculateAppLayout(client_rect);
  const auto source_mode = context->model.configuration().capture.source_mode;
  DrawConfigLabels(hdc, layout.config_rect, source_mode);
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
  const auto old_font = context != nullptr && context->panel_header_font != nullptr
                            ? SelectObject(hdc, context->panel_header_font)
                            : nullptr;

  const auto source_mode = context->model.configuration().capture.source_mode;
  const auto summary_layout = CalculateSummaryPanelLayout(rect, source_mode);
  const int header_left = rect.left + 16;
  const int header_top = rect.top + 10;
  const int subheader_top = rect.top + 26;

  TextOutW(hdc, header_left, header_top, L"Session Snapshot", 16);
  TextOutW(hdc, rect.left + 16, summary_layout.recent_logs_title_y, L"Recent Logs", 11);
  TextOutW(hdc, summary_layout.summary_right_left,
           header_top, L"Configured Summary", 18);
  TextOutW(hdc, summary_layout.summary_right_left,
           summary_layout.diagnostics_title_y, L"Runtime Diagnostics", 19);
  if (old_font != nullptr) {
    SelectObject(hdc, old_font);
  }
  RECT subtitle_rect = {header_left, subheader_top - 1, rect.right - 16, subheader_top + 17};
  DrawTextW(hdc, L"Session state, routing summary, diagnostics, and recent activity.",
            -1, &subtitle_rect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                DT_NOPREFIX);
}

void DrawCapabilityPanel(HDC hdc, const RECT& rect, WindowContext* context) {
  HBRUSH background = CreateSolidBrush(RGB(236, 241, 247));
  FillRect(hdc, &rect, background);
  DeleteObject(background);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(34, 40, 49));
  const auto old_font = context != nullptr && context->panel_header_font != nullptr
                            ? SelectObject(hdc, context->panel_header_font)
                            : nullptr;
  const int header_left = rect.left + 16;
  const int header_top = rect.top + 10;
  const int subheader_top = rect.top + 26;
  TextOutW(hdc, header_left, header_top, L"Runtime Capabilities", 20);
  if (old_font != nullptr) {
    SelectObject(hdc, old_font);
  }
  const auto auto_align_note = BuildAutoAlignExplanatoryNoteText();
  RECT note_rect = {std::max(header_left + 312, static_cast<int>(rect.right) - 380),
                    subheader_top - 1, rect.right - 16, subheader_top + 18};
  RECT subtitle_rect = {header_left, subheader_top - 1, note_rect.left - 12,
                        subheader_top + 18};
  DrawTextW(hdc, L"Backend support, device eligibility, rebuild readiness.", -1,
            &subtitle_rect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
  DrawTextW(hdc, auto_align_note.c_str(), -1, &note_rect,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                DT_NOPREFIX);
}

void DrawProbePanel(HDC hdc, const RECT& rect, WindowContext* context) {
  HBRUSH background = CreateSolidBrush(RGB(230, 238, 246));
  FillRect(hdc, &rect, background);
  DeleteObject(background);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(34, 40, 49));
  const auto old_font = context != nullptr && context->panel_header_font != nullptr
                            ? SelectObject(hdc, context->panel_header_font)
                            : nullptr;
  const int header_left = rect.left + 16;
  TextOutW(hdc, header_left, rect.top + 10, L"Probe Output", 12);
  if (old_font != nullptr) {
    SelectObject(hdc, old_font);
  }
  RECT subtitle_rect = {header_left, rect.top + 25, rect.right - 16, rect.top + 43};
  DrawTextW(hdc, L"Quick/matrix probe results, activation failures, dump state.",
            -1, &subtitle_rect,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                DT_NOPREFIX);
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
      owned_context->app_loopback_process_edit = CreateWindowW(
          L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 16,
          156, 320, 24, hwnd, ControlIdToMenu(kEditAppLoopbackProcess), nullptr,
          nullptr);
      owned_context->delay_edit = CreateWindowW(
          L"EDIT", L"120", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 16,
          176, 100, 24, hwnd, ControlIdToMenu(kEditDelayMs), nullptr,
          nullptr);
      owned_context->dump_checkbox = CreateWindowW(
          L"BUTTON", L"Enable dump", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
          140, 176, 146, 24, hwnd, ControlIdToMenu(kCheckboxDump), nullptr,
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
          L"BUTTON", L"Monitor playback",
          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 424, 326, 170, 24, hwnd,
          ControlIdToMenu(kCheckboxMonitor), nullptr, nullptr);
      owned_context->follow_defaults_checkbox = CreateWindowW(
          L"BUTTON", L"Follow system defaults",
          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 700, 326, 200, 24, hwnd,
          ControlIdToMenu(kCheckboxFollowDefaults), nullptr, nullptr);
      owned_context->auto_align_render_checkbox = CreateWindowW(
          L"BUTTON", L"Auto-align render",
          WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 700, 356, 184, 24, hwnd,
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
      owned_context->visible_snapshot_text = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
              WS_VSCROLL,
          0, 0, 0, 0, hwnd, ControlIdToMenu(kVisibleSnapshotText), nullptr,
          nullptr);
      owned_context->visible_summary_text = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
              WS_VSCROLL,
          0, 0, 0, 0, hwnd, ControlIdToMenu(kVisibleSummaryText), nullptr,
          nullptr);
      owned_context->visible_diagnostics_text = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
              WS_VSCROLL,
          0, 0, 0, 0, hwnd, ControlIdToMenu(kVisibleDiagnosticsText), nullptr,
          nullptr);
      owned_context->visible_capability_text = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
              WS_VSCROLL,
          0, 0, 0, 0, hwnd, ControlIdToMenu(kVisibleCapabilityText), nullptr,
          nullptr);
      owned_context->visible_probe_text = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
              WS_VSCROLL,
          0, 0, 0, 0, hwnd, ControlIdToMenu(kVisibleProbeText), nullptr,
          nullptr);
      owned_context->visible_recent_logs_text = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
              WS_VSCROLL,
          0, 0, 0, 0, hwnd, ControlIdToMenu(kVisibleRecentLogsText), nullptr,
          nullptr);
      owned_context->panel_text_font = CreatePanelTextFont();
      owned_context->panel_mono_font = CreatePanelMonoFont();
      owned_context->panel_header_font = CreatePanelHeaderFont();
      owned_context->panel_edit_brush = CreateSolidBrush(RGB(252, 253, 255));
      owned_context->panel_diagnostic_brush = CreateSolidBrush(RGB(248, 251, 255));
      ConfigureReadOnlyPanelEdit(owned_context->visible_snapshot_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_summary_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_diagnostics_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_capability_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_probe_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_recent_logs_text);
      ApplyPanelTextFont(owned_context->visible_snapshot_text, owned_context->panel_text_font);
      ApplyPanelTextFont(owned_context->visible_summary_text, owned_context->panel_text_font);
      ApplyPanelTextFont(owned_context->visible_diagnostics_text, owned_context->panel_mono_font);
      ApplyPanelTextFont(owned_context->visible_capability_text, owned_context->panel_mono_font);
      ApplyPanelTextFont(owned_context->visible_probe_text, owned_context->panel_mono_font);
      ApplyPanelTextFont(owned_context->visible_recent_logs_text, owned_context->panel_text_font);

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
      LayoutChildControls(hwnd, owned_context);
      owned_context->device_notifications = new DeviceNotificationClient(hwnd);
      owned_context->device_notifications->Register();

      SetTimer(hwnd, kUiTimerId, 33, nullptr);
      SetTimer(hwnd, kAudioTimerId, 10, nullptr);
      return 0;
    }

    case WM_CTLCOLOREDIT:
      if (context != nullptr) {
        const HWND edit = reinterpret_cast<HWND>(l_param);
        const bool is_visible_panel =
            edit == context->visible_snapshot_text ||
            edit == context->visible_recent_logs_text ||
            edit == context->visible_summary_text ||
            edit == context->visible_diagnostics_text ||
            edit == context->visible_capability_text ||
            edit == context->visible_probe_text;
        if (is_visible_panel) {
          auto hdc = reinterpret_cast<HDC>(w_param);
          const bool is_diagnostic_panel =
              edit == context->visible_diagnostics_text ||
              edit == context->visible_capability_text ||
              edit == context->visible_probe_text;
          SetBkColor(hdc, is_diagnostic_panel ? RGB(248, 251, 255)
                                              : RGB(252, 253, 255));
          SetTextColor(hdc, RGB(34, 40, 49));
          SetBkMode(hdc, OPAQUE);
          return reinterpret_cast<INT_PTR>(is_diagnostic_panel
                                               ? context->panel_diagnostic_brush
                                               : context->panel_edit_brush);
        }
      }
      break;

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
                index == 0
                    ? AudioSourceMode::MicrophoneCapture
                    : (index == 1 ? AudioSourceMode::SystemLoopback
                                  : AudioSourceMode::ApplicationLoopback));
            SyncUiFromModel(context);
            LayoutChildControls(hwnd, context);
            InvalidateRect(hwnd, nullptr, TRUE);
          }
          return 0;
        case kEditAppLoopbackProcess:
          if (HIWORD(w_param) == EN_CHANGE) {
            context->model.SetApplicationLoopbackProcess(
                GetWindowTextString(context->app_loopback_process_edit));
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

    case WM_SIZE:
      if (context != nullptr) {
        LayoutChildControls(hwnd, context);
        InvalidateRect(hwnd, nullptr, TRUE);
      }
      return 0;

    case WM_GETMINMAXINFO: {
      auto* minmax = reinterpret_cast<MINMAXINFO*>(l_param);
      RECT desired = {0, 0, kWindowMinClientWidth, kWindowMinClientHeight};
      AdjustWindowRect(&desired, WS_OVERLAPPEDWINDOW, FALSE);
      minmax->ptMinTrackSize.x = desired.right - desired.left;
      minmax->ptMinTrackSize.y = desired.bottom - desired.top;
      return 0;
    }

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
        const AppLayout layout = CalculateAppLayout(client_rect);
        DrawSummary(hdc, layout.summary_rect, context);
        DrawCapabilityPanel(hdc, layout.capability_rect, context);
        DrawProbePanel(hdc, layout.probe_rect, context);
        DrawConfigLabels(hdc, context);

        WaveformRenderer::Draw(hdc, layout.capture_waveform_rect,
                               context->model.capture_waveform(),
                               RGB(41, 182, 246), L"Capture Waveform",
                               context->model.stats().capture_meter);
        WaveformRenderer::Draw(hdc, layout.render_waveform_rect,
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
        if (context->panel_text_font != nullptr &&
            context->panel_text_font != GetStockObject(DEFAULT_GUI_FONT)) {
          DeleteObject(context->panel_text_font);
          context->panel_text_font = nullptr;
        }
        if (context->panel_mono_font != nullptr) {
          DeleteObject(context->panel_mono_font);
          context->panel_mono_font = nullptr;
        }
        if (context->panel_header_font != nullptr) {
          DeleteObject(context->panel_header_font);
          context->panel_header_font = nullptr;
        }
        if (context->panel_edit_brush != nullptr) {
          DeleteObject(context->panel_edit_brush);
          context->panel_edit_brush = nullptr;
        }
        if (context->panel_diagnostic_brush != nullptr) {
          DeleteObject(context->panel_diagnostic_brush);
          context->panel_diagnostic_brush = nullptr;
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
