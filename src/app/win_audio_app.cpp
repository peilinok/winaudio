#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <cwchar>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "app_model.h"
#include "probe_ui_text.h"
#include "device_notification_client.h"
#include "audio/backends/real_backends.h"
#include "audio/com_support.h"
#include "ui/waveform_renderer.h"

namespace winaudio {

namespace {

constexpr wchar_t kWindowClassName[] = L"WinAudioDemoWindowClass";
constexpr UINT_PTR kUiTimerId = 1;
constexpr UINT_PTR kAudioTimerId = 2;
constexpr UINT kMessageProbeFinished = WM_APP + 2;
constexpr UINT kMessageProbeMatrixFinished = WM_APP + 3;
constexpr UINT kMessageComboSetMinVisible = 0x1701;

constexpr int kWindowMinClientWidth = 1120;
constexpr int kWindowMinClientHeight = 980;
constexpr int kOuterMargin = 16;
constexpr int kSectionGap = 14;
constexpr int kButtonHeight = 30;
constexpr int kButtonGap = 12;
constexpr int kToolbarTitleWidth = 138;
constexpr int kComboHeight = 24;
constexpr int kComboMinVisibleItems = 12;
constexpr int kEditHeight = 24;
constexpr int kSummaryMinHeight = 196;
constexpr int kCapabilityMinHeight = 88;
constexpr int kProbeMinHeight = 104;
constexpr int kWaveformMinHeight = 96;
constexpr int kLogsMinHeight = 120;
constexpr int kOverviewMinPageHeight = 236;
constexpr int kInfoTabsExtraHeightShareNumerator = 2;
constexpr int kInfoTabsExtraHeightShareDenominator = 5;
constexpr int kPanelInset = 16;
constexpr int kPanelGap = 12;
constexpr int kPanelHeaderHeight = 24;

constexpr int kButtonStart = 1001;
constexpr int kButtonStop = 1002;
constexpr int kButtonRefresh = 1003;
constexpr int kButtonProbe = 1004;
constexpr int kButtonProbeMatrix = 1005;
constexpr int kButtonCaptureOpenProbe = 1030;
constexpr int kTabInfoPages = 1006;
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
constexpr int kComboCaptureCategory = 1126;
constexpr int kComboCaptureOptions = 1127;
constexpr int kComboRenderShareMode = 1123;
constexpr int kComboRenderDriveMode = 1124;
constexpr int kComboRenderCategory = 1128;
constexpr int kComboRenderOptions = 1129;
constexpr int kEditAppLoopbackProcess = 1125;
constexpr int kEditDumpPath = 1113;
constexpr int kComboDumpType = 1114;
constexpr int kEditCaptureBufferMs = 1115;
constexpr int kEditRenderBufferMs = 1116;
constexpr int kCheckboxMonitor = 1117;
constexpr int kCheckboxFollowDefaults = 1118;
constexpr int kCheckboxAutoAlignRender = 1122;
constexpr int kButtonRtcJoinLeave = 1132;
constexpr int kStaticRtcStatus = 1133;
constexpr int kEditRtcAppId = 1134;
constexpr int kEditRtcChannel = 1135;
constexpr int kEditRtcUid = 1136;
constexpr int kEditRtcToken = 1137;
constexpr int kButtonLogsConsoleToggle = 1138;
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
constexpr int kOverviewSnapshotLabel = 1956;
constexpr int kOverviewSummaryLabel = 1957;
constexpr int kVisibleRtcText = 1959;

struct WindowContext {
  ScopedCoInitialize com {};
  AppModel model {};
  HWND start_button = nullptr;
  HWND stop_button = nullptr;
  HWND refresh_button = nullptr;
  HWND probe_button = nullptr;
  HWND probe_matrix_button = nullptr;
  HWND capture_open_probe_button = nullptr;
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
  HWND capture_category_combo = nullptr;
  HWND capture_options_combo = nullptr;
  HWND render_share_mode_combo = nullptr;
  HWND render_drive_mode_combo = nullptr;
  HWND render_category_combo = nullptr;
  HWND render_options_combo = nullptr;
  HWND app_loopback_process_edit = nullptr;
  HWND dump_path_edit = nullptr;
  HWND dump_type_combo = nullptr;
  HWND capture_buffer_edit = nullptr;
  HWND render_buffer_edit = nullptr;
  HWND monitor_checkbox = nullptr;
  HWND follow_defaults_checkbox = nullptr;
  HWND auto_align_render_checkbox = nullptr;
  HWND rtc_join_leave_button = nullptr;
  HWND rtc_status_label = nullptr;
  HWND rtc_app_id_edit = nullptr;
  HWND rtc_channel_edit = nullptr;
  HWND rtc_uid_edit = nullptr;
  HWND rtc_token_edit = nullptr;
  HWND automation_capture_label = nullptr;
  HWND automation_device_count_line = nullptr;
  HWND automation_summary_text = nullptr;
  HWND automation_diagnostics_text = nullptr;
  HWND automation_probe_text = nullptr;
  HWND automation_auto_align_note = nullptr;
  HWND info_tabs = nullptr;
  HWND overview_snapshot_label = nullptr;
  HWND overview_summary_label = nullptr;
  HWND visible_snapshot_text = nullptr;
  HWND visible_summary_text = nullptr;
  HWND visible_diagnostics_text = nullptr;
  HWND visible_capability_text = nullptr;
  HWND visible_probe_text = nullptr;
  HWND logs_console_toggle_button = nullptr;
  HWND visible_recent_logs_text = nullptr;
  HWND visible_rtc_text = nullptr;
  HFONT panel_text_font = nullptr;
  HFONT panel_mono_font = nullptr;
  HFONT panel_header_font = nullptr;
  HBRUSH panel_edit_brush = nullptr;
  HBRUSH panel_diagnostic_brush = nullptr;
  std::wstring last_window_title {};
  std::wstring last_automation_capture_label {};
  std::wstring last_automation_device_count_line {};
  std::wstring last_automation_summary_text {};
  std::wstring last_automation_diagnostics_text {};
  std::wstring last_automation_probe_text {};
  std::wstring last_automation_auto_align_note {};
  std::wstring last_visible_snapshot_text {};
  std::wstring last_visible_summary_text {};
  std::wstring last_visible_diagnostics_text {};
  std::wstring last_visible_capability_text {};
  std::wstring last_visible_probe_text {};
  std::wstring last_visible_recent_logs_text {};
  std::wstring last_visible_rtc_text {};
  HWND console_window = nullptr;
  HANDLE console_output_handle = nullptr;
  uint64_t last_console_log_count = 0;
  DeviceNotificationClient* device_notifications = nullptr;
  bool probe_running = false;
  ProbeUiMode probe_mode = ProbeUiMode::None;
  std::jthread probe_thread {};
  bool shutting_down = false;
  bool logs_console_open = false;
  bool audio_timer_enabled = false;
  bool ui_timer_enabled = false;
};

struct AppLayout {
  RECT config_rect {};
  RECT info_tabs_rect {};
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
  int category_width = 0;
  int options_width = 0;
  int format_row1_y = 0;
  int format_row2_y = 0;
  int format_row3_y = 0;
  int format_row1_label_y = 0;
  int format_row2_label_y = 0;
  int format_row3_label_y = 0;
  int output_left = 0;
  int output_inner_width = 0;
  int output_left_width = 0;
  int output_right_left = 0;
  int output_right_width = 0;
  int output_label_y = 0;
  int output_label_y2 = 0;
  int output_label_y3 = 0;
  int row5_y = 0;
  int row6_y = 0;
  int row7_y = 0;
  int rtc_row1_y = 0;
  int rtc_row2_y = 0;
  int rtc_row3_y = 0;
  int rtc_token_y = 0;
  int rtc_uid_width = 92;
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

void SetWindowTextIfChanged(HWND hwnd,
                            std::wstring& cache,
                            const std::wstring& text) {
  if (hwnd == nullptr || cache == text) {
    return;
  }
  SetWindowTextW(hwnd, text.c_str());
  cache = text;
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

void ConfigureComboDropdown(HWND hwnd) {
  if (hwnd == nullptr) {
    return;
  }
  SendMessageW(hwnd, kMessageComboSetMinVisible,
               static_cast<WPARAM>(kComboMinVisibleItems), 0);
}

RECT GetTabPageRect(HWND tab_control) {
  RECT page_rect {};
  if (tab_control == nullptr) {
    return page_rect;
  }
  GetClientRect(tab_control, &page_rect);
  TabCtrl_AdjustRect(tab_control, FALSE, &page_rect);
  MapWindowPoints(tab_control, GetParent(tab_control),
                  reinterpret_cast<POINT*>(&page_rect), 2);
  return page_rect;
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

bool IsSessionActive(const WindowContext* context) {
  return context != nullptr && context->model.session_state() == L"Running";
}

bool ShouldRunAudioTimer(const WindowContext* context) {
  return context != nullptr &&
         (context->probe_running || IsSessionActive(context));
}

bool ShouldRunUiTimer(const WindowContext* context) {
  return context != nullptr &&
         (context->probe_running || IsSessionActive(context) ||
          context->logs_console_open);
}

std::wstring BuildLogsConsoleButtonText(bool console_open) {
  return console_open ? L"Close Console" : L"Open Console";
}

std::wstring BuildRecentLogsPanelText(const LogSnapshot& snapshot) {
  if (snapshot.lines.empty()) {
    return L"No recent logs yet.";
  }

  std::wstring log_text;
  for (auto it = snapshot.lines.rbegin(); it != snapshot.lines.rend(); ++it) {
    if (!log_text.empty()) {
      log_text += L"\r\n";
    }
    log_text += *it;
  }
  return log_text;
}

void UpdateLogsConsoleButtonText(WindowContext* context) {
  if (context == nullptr || context->logs_console_toggle_button == nullptr) {
    return;
  }
  SetControlText(context->logs_console_toggle_button,
                 BuildLogsConsoleButtonText(context->logs_console_open).c_str());
}

void DisableConsoleCloseButton(HWND console_window) {
  if (console_window == nullptr) {
    return;
  }
  HMENU system_menu = GetSystemMenu(console_window, FALSE);
  if (system_menu == nullptr) {
    return;
  }
  EnableMenuItem(system_menu, SC_CLOSE, MF_BYCOMMAND | MF_GRAYED);
  DrawMenuBar(console_window);
}

void CloseLogsConsole(WindowContext* context) {
  if (context == nullptr) {
    return;
  }

  context->console_output_handle = nullptr;
  context->console_window = nullptr;
  context->last_console_log_count = 0;
  if (context->logs_console_open) {
    FreeConsole();
  }
  context->logs_console_open = false;
  UpdateLogsConsoleButtonText(context);
}

bool OpenLogsConsole(WindowContext* context, HWND owner) {
  if (context == nullptr) {
    return false;
  }
  if (context->logs_console_open) {
    if (context->console_window != nullptr) {
      ShowWindow(context->console_window, SW_SHOW);
      SetForegroundWindow(context->console_window);
    }
    return true;
  }

  if (!AllocConsole()) {
    MessageBoxW(owner, L"Failed to open the logs console.", L"WinAudio",
                MB_OK | MB_ICONERROR);
    return false;
  }

  SetConsoleTitleW(L"WinAudio Logs");
  context->console_window = GetConsoleWindow();
  DisableConsoleCloseButton(context->console_window);
  context->console_output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
  if (context->console_output_handle == nullptr ||
      context->console_output_handle == INVALID_HANDLE_VALUE) {
    MessageBoxW(owner, L"Failed to initialize the logs console output handle.",
                L"WinAudio", MB_OK | MB_ICONERROR);
    CloseLogsConsole(context);
    return false;
  }

  context->last_console_log_count = context->model.log_snapshot().total_count;
  context->logs_console_open = true;
  UpdateLogsConsoleButtonText(context);
  return true;
}

bool WriteRecentLogsToConsole(WindowContext* context, const LogSnapshot& snapshot) {
  if (context == nullptr || !context->logs_console_open ||
      context->console_output_handle == nullptr ||
      context->console_output_handle == INVALID_HANDLE_VALUE) {
    return false;
  }

  if (snapshot.total_count <= context->last_console_log_count) {
    context->last_console_log_count = snapshot.total_count;
    return false;
  }

  const uint64_t new_log_count =
      snapshot.total_count - context->last_console_log_count;
  const size_t available_lines = snapshot.lines.size();
  const size_t lines_to_write = static_cast<size_t>(
      std::min<uint64_t>(new_log_count, static_cast<uint64_t>(available_lines)));
  if (lines_to_write == 0) {
    context->last_console_log_count = snapshot.total_count;
    return false;
  }

  const size_t start_index = available_lines - lines_to_write;
  bool wrote_anything = false;
  for (size_t index = start_index; index < available_lines; ++index) {
    const std::wstring line = snapshot.lines[index] + L"\r\n";
    DWORD written = 0;
    if (!WriteConsoleW(context->console_output_handle, line.c_str(),
                       static_cast<DWORD>(line.size()), &written, nullptr)) {
      CloseLogsConsole(context);
      return false;
    }
    wrote_anything = true;
  }
  context->last_console_log_count = snapshot.total_count;
  return wrote_anything;
}

void UpdateTimerState(HWND hwnd, WindowContext* context) {
  if (context == nullptr) {
    return;
  }

  const bool should_run_audio_timer = ShouldRunAudioTimer(context);
  if (should_run_audio_timer != context->audio_timer_enabled) {
    if (should_run_audio_timer) {
      SetTimer(hwnd, kAudioTimerId, 10, nullptr);
    } else {
      KillTimer(hwnd, kAudioTimerId);
    }
    context->audio_timer_enabled = should_run_audio_timer;
  }

  const bool should_run_ui_timer = ShouldRunUiTimer(context);
  if (should_run_ui_timer != context->ui_timer_enabled) {
    if (should_run_ui_timer) {
      SetTimer(hwnd, kUiTimerId, 33, nullptr);
    } else {
      KillTimer(hwnd, kUiTimerId);
    }
    context->ui_timer_enabled = should_run_ui_timer;
  }
}

AppLayout CalculateAppLayout(const RECT& client_rect) {
  AppLayout layout {};
  const int client_width = std::max(RectWidth(client_rect), kWindowMinClientWidth);
  const int client_height =
      std::max(RectHeight(client_rect), kWindowMinClientHeight);
  const int inner_width = client_width - (kOuterMargin * 2);

  const int config_height = 570;
  const int vertical_budget =
      client_height - (kOuterMargin * 2) - config_height - (kSectionGap * 3);
  const int info_tabs_min_height =
      std::max({kOverviewMinPageHeight, kSummaryMinHeight, kCapabilityMinHeight,
                kProbeMinHeight, kLogsMinHeight});
  const int min_content_height = info_tabs_min_height + (kWaveformMinHeight * 2);
  const int extra_height = std::max(0, vertical_budget - min_content_height);
  const int info_tabs_height =
      info_tabs_min_height +
      ((extra_height * kInfoTabsExtraHeightShareNumerator) /
       kInfoTabsExtraHeightShareDenominator);
  const int waveform_extra =
      std::max(0, vertical_budget - info_tabs_height - (kWaveformMinHeight * 2));
  const int capture_waveform_height = kWaveformMinHeight + (waveform_extra / 2);
  const int render_waveform_height =
      kWaveformMinHeight + (waveform_extra - (waveform_extra / 2));

  layout.config_rect = MakeRect(kOuterMargin, kOuterMargin, inner_width, config_height);
  int top = layout.config_rect.bottom + kSectionGap;
  layout.info_tabs_rect = MakeRect(kOuterMargin, top, inner_width, info_tabs_height);
  top = layout.info_tabs_rect.bottom + kSectionGap;

  layout.capture_waveform_rect =
      MakeRect(kOuterMargin, top, inner_width, capture_waveform_height);
  top = layout.capture_waveform_rect.bottom + kSectionGap;
  layout.render_waveform_rect =
      MakeRect(kOuterMargin, top, inner_width, render_waveform_height);

  return layout;
}

enum class InfoTabPage {
  Overview = 0,
  Diagnostics = 1,
  Rtc = 2,
  Capabilities = 3,
  Probe = 4,
  Logs = 5,
};

InfoTabPage GetActiveInfoTabPage(WindowContext* context) {
  if (context == nullptr || context->info_tabs == nullptr) {
    return InfoTabPage::Overview;
  }
  const int selection = TabCtrl_GetCurSel(context->info_tabs);
  if (selection < 0) {
    return InfoTabPage::Overview;
  }
  return static_cast<InfoTabPage>(selection);
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

  layout.toolbar_rect = MakeRect(rect.left, rect.top, content_width, 46);
  layout.routing_rect = MakeRect(rect.left, rect.top + 54, content_width, 132);
  layout.capture_format_rect =
      MakeRect(rect.left, rect.top + 194, format_width, 190);
  layout.render_format_rect =
      MakeRect(rect.left + format_width + format_gap, rect.top + 194,
               content_width - format_width - format_gap, 190);
  layout.output_rect =
      MakeRect(rect.left, rect.top + 392, content_width,
               rect.bottom - (rect.top + 392));

  layout.route_column_width =
      (RectWidth(layout.routing_rect) - (section_inset * 2) - inner_gap) / 2;
  layout.route_left = layout.routing_rect.left + section_inset;
  layout.route_right = layout.route_left + layout.route_column_width + inner_gap;
  layout.backend_width = 180;
  layout.source_width = layout.route_column_width - layout.backend_width - inner_gap;
  layout.row1_label_y = layout.routing_rect.top + 32;
  layout.row1_y = layout.routing_rect.top + 50;
  layout.row2_label_y = layout.routing_rect.top + 76;
  layout.row2_y = layout.routing_rect.top + 94;

  layout.rate_width = 110;
  layout.channels_width = 82;
  layout.type_width = RectWidth(layout.capture_format_rect) - (section_inset * 2) -
                      layout.rate_width - layout.channels_width - (inner_gap * 2);
  layout.mode_width =
      (RectWidth(layout.capture_format_rect) - (section_inset * 2) - inner_gap) / 2;
  layout.category_width =
      (RectWidth(layout.capture_format_rect) - (section_inset * 2) - inner_gap) / 2;
  layout.options_width = layout.category_width;
  layout.format_row1_label_y = layout.capture_format_rect.top + 28;
  layout.format_row1_y = layout.capture_format_rect.top + 46;
  layout.format_row2_label_y = layout.capture_format_rect.top + 88;
  layout.format_row2_y = layout.capture_format_rect.top + 106;
  layout.format_row3_label_y = layout.capture_format_rect.top + 134;
  layout.format_row3_y = layout.capture_format_rect.top + 152;

  layout.output_left = layout.output_rect.left + section_inset;
  layout.output_inner_width = RectWidth(layout.output_rect) - (section_inset * 2);
  layout.output_left_width = std::clamp((layout.output_inner_width * 50) / 100,
                                        580, layout.output_inner_width - 360);
  layout.output_right_left = layout.output_left + layout.output_left_width + inner_gap;
  layout.output_right_width =
      layout.output_inner_width - layout.output_left_width - inner_gap;
  layout.output_label_y = layout.output_rect.top + 30;
  layout.row5_y = layout.output_rect.top + 48;
  layout.output_label_y2 = layout.output_rect.top + 74;
  layout.row6_y = layout.output_rect.top + 92;
  layout.output_label_y3 = layout.output_rect.top + 118;
  layout.row7_y = layout.output_rect.top + 136;
  layout.rtc_row1_y = layout.output_rect.top + 38;
  layout.rtc_row2_y = layout.output_rect.top + 82;
  layout.rtc_row3_y = layout.output_rect.top + 116;
  layout.rtc_token_y = layout.output_rect.top + 150;
  layout.dump_checkbox_width = 146;
  layout.dump_type_width = 160;
  layout.dump_path_width =
      layout.output_left_width - layout.dump_checkbox_width -
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
  const int toolbar_button_left =
      config_layout.toolbar_rect.left + kToolbarTitleWidth + 16;
  const int toolbar_right = config_layout.toolbar_rect.right - 8;
  const int available_button_width =
      std::max(0, toolbar_right - toolbar_button_left - (kButtonGap * 5));
  const int start_width = std::clamp((available_button_width * 17) / 100, 96, 128);
  const int stop_width = std::clamp((available_button_width * 12) / 100, 72, 92);
  const int refresh_width =
      std::clamp((available_button_width * 18) / 100, 112, 140);
  const int quick_probe_width =
      std::clamp((available_button_width * 19) / 100, 118, 150);
  const int matrix_width =
      std::clamp((available_button_width * 20) / 100, 128, 162);
  const int capture_open_width =
      std::clamp(available_button_width - start_width - stop_width -
                     refresh_width - quick_probe_width - matrix_width,
                 154, 178);

  int x = toolbar_button_left;
  MoveControl(context->start_button,
              MakeRect(x, button_y, start_width, kButtonHeight));
  x += start_width + kButtonGap;
  MoveControl(context->stop_button, MakeRect(x, button_y, stop_width, kButtonHeight));
  x += stop_width + kButtonGap;
  MoveControl(context->refresh_button,
              MakeRect(x, button_y, refresh_width, kButtonHeight));
  x += refresh_width + kButtonGap;
  MoveControl(context->probe_button,
              MakeRect(x, button_y, quick_probe_width, kButtonHeight));
  x += quick_probe_width + kButtonGap;
  MoveControl(context->probe_matrix_button,
              MakeRect(x, button_y, matrix_width, kButtonHeight));
  x += matrix_width + kButtonGap;
  MoveControl(context->capture_open_probe_button,
              MakeRect(x, button_y, capture_open_width, kButtonHeight));

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
  x = config_layout.capture_format_rect.left + 16;
  MoveControl(context->capture_category_combo,
              MakeRect(x, config_layout.format_row3_y, config_layout.category_width,
                       kComboHeight));
  x += config_layout.category_width + inner_gap;
  MoveControl(context->capture_options_combo,
              MakeRect(x, config_layout.format_row3_y, config_layout.options_width,
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
  x = config_layout.render_format_rect.left + 16;
  MoveControl(context->render_category_combo,
              MakeRect(x, config_layout.format_row3_y, config_layout.category_width,
                       kComboHeight));
  x += config_layout.category_width + inner_gap;
  MoveControl(context->render_options_combo,
              MakeRect(x, config_layout.format_row3_y, config_layout.options_width,
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

  MoveControl(context->rtc_join_leave_button,
              MakeRect(config_layout.output_right_left, config_layout.rtc_row1_y,
                       132, kButtonHeight));
  MoveControl(context->rtc_status_label,
              MakeRect(config_layout.output_right_left + 144, config_layout.rtc_row1_y + 4,
                       config_layout.output_right_width - 144, 20));
  MoveControl(context->rtc_app_id_edit,
              MakeRect(config_layout.output_right_left, config_layout.rtc_row2_y,
                       config_layout.output_right_width, kEditHeight));
  MoveControl(context->rtc_channel_edit,
              MakeRect(config_layout.output_right_left, config_layout.rtc_row3_y,
                       config_layout.output_right_width - config_layout.rtc_uid_width -
                           inner_gap,
                       kEditHeight));
  MoveControl(context->rtc_uid_edit,
              MakeRect(config_layout.output_right_left + config_layout.output_right_width -
                           config_layout.rtc_uid_width,
                       config_layout.rtc_row3_y, config_layout.rtc_uid_width,
                       kEditHeight));
  MoveControl(context->rtc_token_edit,
              MakeRect(config_layout.output_right_left, config_layout.rtc_token_y,
                       config_layout.output_right_width, kEditHeight));

  MoveControl(context->info_tabs, layout.info_tabs_rect);
  const RECT page_rect = GetTabPageRect(context->info_tabs);
  const int page_width = RectWidth(page_rect);
  const int page_height = RectHeight(page_rect);
  const int page_left = page_rect.left;
  const int page_top = page_rect.top;
  const int page_inset = 16;
  const int label_height = 18;
  const int column_width = std::max(220, (page_width - (page_inset * 2) - inner_gap) / 2);
  const int right_left = page_left + page_inset + column_width + inner_gap;
  const int right_width =
      std::max(220, page_width - (page_inset * 2) - column_width - inner_gap);

  const int overview_header_top = page_top + page_inset;
  const int overview_text_top = overview_header_top + 22;
  const int overview_text_height =
      std::max(80, page_height - (page_inset * 2) - 22);
  const int logs_button_width = 124;
  const int logs_button_height = kButtonHeight;
  const int logs_header_top = page_top + page_inset;
  const int logs_text_top = logs_header_top + logs_button_height + 12;
  const int logs_text_height =
      std::max(60, page_top + page_height - page_inset - logs_text_top);

  MoveControl(context->overview_snapshot_label,
              MakeRect(page_left + page_inset, overview_header_top, column_width,
                       label_height));
  MoveControl(context->visible_snapshot_text,
              MakeRect(page_left + page_inset, overview_text_top, column_width,
                       overview_text_height));
  MoveControl(context->overview_summary_label,
              MakeRect(right_left, overview_header_top, right_width, label_height));
  MoveControl(context->visible_summary_text,
              MakeRect(right_left, overview_text_top, right_width, overview_text_height));
  MoveControl(context->logs_console_toggle_button,
              MakeRect(page_left + page_width - page_inset - logs_button_width,
                       logs_header_top, logs_button_width, logs_button_height));
  MoveControl(context->visible_recent_logs_text,
              MakeRect(page_left + page_inset, logs_text_top,
                       page_width - (page_inset * 2), logs_text_height));
  MoveControl(context->visible_capability_text,
              MakeRect(page_left + page_inset, page_top + page_inset,
                       page_width - (page_inset * 2), page_height - (page_inset * 2)));
  MoveControl(context->visible_probe_text,
              MakeRect(page_left + page_inset, page_top + page_inset,
                       page_width - (page_inset * 2), page_height - (page_inset * 2)));
  MoveControl(context->visible_rtc_text,
              MakeRect(page_left + page_inset, page_top + page_inset,
                       page_width - (page_inset * 2), page_height - (page_inset * 2)));

  const auto active_page = GetActiveInfoTabPage(context);
  const bool show_overview = active_page == InfoTabPage::Overview;
  const bool show_diagnostics = active_page == InfoTabPage::Diagnostics;
  const bool show_rtc = active_page == InfoTabPage::Rtc;
  const bool show_capabilities = active_page == InfoTabPage::Capabilities;
  const bool show_probe = active_page == InfoTabPage::Probe;
  const bool show_logs = active_page == InfoTabPage::Logs;

  SetControlVisible(context->overview_snapshot_label, show_overview);
  SetControlVisible(context->overview_summary_label, show_overview);
  SetControlVisible(context->visible_snapshot_text, show_overview);
  SetControlVisible(context->visible_summary_text, show_overview);
  SetControlVisible(context->logs_console_toggle_button, show_logs);
  SetControlVisible(context->visible_recent_logs_text, show_logs);
  SetControlVisible(context->visible_diagnostics_text, show_diagnostics);
  SetControlVisible(context->visible_rtc_text, show_rtc);
  SetControlVisible(context->visible_capability_text, show_capabilities);
  SetControlVisible(context->visible_probe_text, show_probe);

  if (show_diagnostics) {
    MoveControl(context->visible_diagnostics_text,
                MakeRect(page_left + page_inset, page_top + page_inset,
                         page_width - (page_inset * 2), page_height - (page_inset * 2)));
  }

  UpdateReadOnlyPanelTextRect(context->visible_snapshot_text);
  UpdateReadOnlyPanelTextRect(context->visible_recent_logs_text);
  UpdateReadOnlyPanelTextRect(context->visible_summary_text);
  UpdateReadOnlyPanelTextRect(context->visible_diagnostics_text);
  UpdateReadOnlyPanelTextRect(context->visible_rtc_text);
  UpdateReadOnlyPanelTextRect(context->visible_capability_text);
  UpdateReadOnlyPanelTextRect(context->visible_probe_text);
}

void ApplyControlAvailability(WindowContext* context) {
  const auto config = context->model.configuration();
  const auto session_state = context->model.session_state();
  const bool busy = context->probe_running;
  const bool session_running = session_state == L"Running";
  const bool monitor_enabled = config.render.monitor_enabled;
  const bool application_loopback =
      config.capture.source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      config.capture.source_mode == AudioSourceMode::ApplicationLoopback;
  const bool system_loopback =
      config.capture.source_mode == AudioSourceMode::SystemLoopback;
  const BOOL general_enabled = busy ? FALSE : TRUE;
  const BOOL source_mode_enabled =
      (!busy && !session_running) ? TRUE : FALSE;
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
  const auto rtc_stats = context->model.rtc_stats();
  const bool rtc_runtime_available = IsRtcRuntimeAvailable(rtc_stats);
  const BOOL rtc_config_edit_enabled =
      (!busy && !rtc_stats.joined && rtc_runtime_available) ? TRUE : FALSE;
  const BOOL rtc_button_enabled =
      (!busy && rtc_runtime_available) ? TRUE : FALSE;

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
      context->capture_open_probe_button,
      context->capture_backend_combo,
      context->app_loopback_process_edit,
      context->dump_checkbox,
      context->capture_sample_rate_combo,
      context->capture_channels_combo,
      context->capture_sample_type_combo,
      context->capture_share_mode_combo,
      context->capture_drive_mode_combo,
      context->capture_category_combo,
      context->capture_options_combo,
      context->dump_path_edit,
      context->dump_type_combo,
      context->capture_buffer_edit,
      context->follow_defaults_checkbox,
  };
  for (HWND control : general_controls) {
    if (control != nullptr) {
      EnableWindow(control, general_enabled);
    }
  }
  if (context->source_mode_combo != nullptr) {
    EnableWindow(context->source_mode_combo, source_mode_enabled);
  }
  if (context->capture_device_combo != nullptr) {
    EnableWindow(context->capture_device_combo, capture_device_combo_enabled);
  }
  if (context->app_loopback_process_edit != nullptr) {
    EnableWindow(context->app_loopback_process_edit,
                 application_loopback_target_enabled);
  }
  if (context->monitor_checkbox != nullptr) {
    EnableWindow(context->monitor_checkbox,
                 (!busy && !system_loopback) ? TRUE : FALSE);
  }
  if (context->rtc_join_leave_button != nullptr) {
    EnableWindow(context->rtc_join_leave_button, rtc_button_enabled);
  }
  if (context->rtc_app_id_edit != nullptr) {
    EnableWindow(context->rtc_app_id_edit, rtc_config_edit_enabled);
  }
  if (context->rtc_channel_edit != nullptr) {
    EnableWindow(context->rtc_channel_edit, rtc_config_edit_enabled);
  }
  if (context->rtc_uid_edit != nullptr) {
    EnableWindow(context->rtc_uid_edit, rtc_config_edit_enabled);
  }
  if (context->rtc_token_edit != nullptr) {
    EnableWindow(context->rtc_token_edit, rtc_config_edit_enabled);
  }
  if (context->render_device_combo != nullptr) {
    EnableWindow(context->render_device_combo, render_device_combo_enabled);
  }
  const HWND render_pipeline_controls[] = {
      context->render_backend_combo,
      context->delay_edit,
      context->render_share_mode_combo,
      context->render_drive_mode_combo,
      context->render_category_combo,
      context->render_options_combo,
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
  SetWindowTextW(context->capture_open_probe_button,
                 BuildCaptureOpenProbeButtonLabel(busy).c_str());
}

bool SyncAutomationTextMirrors(WindowContext* context) {
  if (context == nullptr) {
    return false;
  }

  bool changed = false;
  const auto log_snapshot = context->model.log_snapshot();

  const auto devices = context->model.devices();
  const auto source_mode = context->model.configuration().capture.source_mode;

  if (context->automation_capture_label != nullptr) {
    const auto text = BuildCaptureDeviceLabelText(source_mode);
    const auto previous = context->last_automation_capture_label;
    SetWindowTextIfChanged(context->automation_capture_label,
                           context->last_automation_capture_label, text);
    changed = changed || previous != text;
  }
  if (context->automation_device_count_line != nullptr) {
    const auto text = BuildDeviceCountLineText(source_mode, devices.capture_devices.size(),
                                               devices.render_devices.size());
    const auto previous = context->last_automation_device_count_line;
    SetWindowTextIfChanged(context->automation_device_count_line,
                           context->last_automation_device_count_line, text);
    changed = changed || previous != text;
  }
  if (context->automation_summary_text != nullptr) {
    const auto text = context->model.summary_text();
    const auto previous = context->last_automation_summary_text;
    SetWindowTextIfChanged(context->automation_summary_text,
                           context->last_automation_summary_text, text);
    changed = changed || previous != text;
  }
  if (context->visible_snapshot_text != nullptr) {
    const auto previous = context->last_visible_snapshot_text;
    SetEditTextIfChanged(context->visible_snapshot_text,
                         context->last_visible_snapshot_text,
                         BuildSessionSnapshotText(context));
    changed = changed || previous != context->last_visible_snapshot_text;
  }
  if (context->visible_summary_text != nullptr) {
    const auto previous = context->last_visible_summary_text;
    SetEditTextIfChanged(context->visible_summary_text,
                         context->last_visible_summary_text,
                         FormatSummaryPanelText(context->model.summary_text()));
    changed = changed || previous != context->last_visible_summary_text;
  }
  if (context->automation_diagnostics_text != nullptr) {
    const auto text = context->model.diagnostics_text();
    const auto previous = context->last_automation_diagnostics_text;
    SetWindowTextIfChanged(context->automation_diagnostics_text,
                           context->last_automation_diagnostics_text, text);
    changed = changed || previous != text;
  }
  if (context->visible_diagnostics_text != nullptr) {
    const auto previous = context->last_visible_diagnostics_text;
    SetEditTextIfChanged(context->visible_diagnostics_text,
                         context->last_visible_diagnostics_text,
                         FormatDiagnosticsPanelText(context->model.diagnostics_text()));
    changed = changed || previous != context->last_visible_diagnostics_text;
  }
  if (context->automation_probe_text != nullptr) {
    const auto text = context->model.probe_text();
    const auto previous = context->last_automation_probe_text;
    SetWindowTextIfChanged(context->automation_probe_text,
                           context->last_automation_probe_text, text);
    changed = changed || previous != text;
  }
  if (context->visible_probe_text != nullptr) {
    const auto previous = context->last_visible_probe_text;
    SetEditTextIfChanged(context->visible_probe_text,
                         context->last_visible_probe_text,
                         FormatProbePanelText(context->model.probe_text()));
    changed = changed || previous != context->last_visible_probe_text;
  }
  if (context->visible_recent_logs_text != nullptr) {
    const auto previous = context->last_visible_recent_logs_text;
    SetEditTextIfChanged(context->visible_recent_logs_text,
                         context->last_visible_recent_logs_text,
                         BuildRecentLogsPanelText(log_snapshot));
    changed = changed || previous != context->last_visible_recent_logs_text;
  }
  WriteRecentLogsToConsole(context, log_snapshot);
  if (context->automation_auto_align_note != nullptr) {
    const auto text = BuildAutoAlignExplanatoryNoteText();
    const auto previous = context->last_automation_auto_align_note;
    SetWindowTextIfChanged(context->automation_auto_align_note,
                           context->last_automation_auto_align_note, text);
    changed = changed || previous != text;
  }
  if (context->visible_capability_text != nullptr) {
    const auto previous = context->last_visible_capability_text;
    SetEditTextIfChanged(context->visible_capability_text,
                         context->last_visible_capability_text,
                         FormatCapabilityPanelText(context->model.capability_text()));
    changed = changed || previous != context->last_visible_capability_text;
  }
  if (context->visible_rtc_text != nullptr) {
    const auto previous = context->last_visible_rtc_text;
    SetEditTextIfChanged(context->visible_rtc_text,
                         context->last_visible_rtc_text,
                         context->model.rtc_text());
    changed = changed || previous != context->last_visible_rtc_text;
  }
  if (context->rtc_join_leave_button != nullptr &&
      context->rtc_status_label != nullptr) {
    const auto config = context->model.configuration();
    const auto rtc_stats = context->model.rtc_stats();
    SetControlText(context->rtc_join_leave_button,
                   BuildRtcJoinButtonLabelText(config.rtc, rtc_stats).c_str());
    SetControlText(context->rtc_status_label,
                   BuildRtcStatusLabelText(config, context->model.session_state(),
                                           rtc_stats).c_str());
  }
  UpdateLogsConsoleButtonText(context);

  return changed;
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
  if (WasapiCaptureAdapter::IsProcessLoopbackSupportedOnCurrentWindows()) {
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Loopback Process"));
    SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Loopback Application"));
  }
}

int SourceModeToComboIndex(AudioSourceMode source_mode) {
  if (source_mode == AudioSourceMode::MicrophoneCapture) {
    return 0;
  }
  if (source_mode == AudioSourceMode::SystemLoopback) {
    return 1;
  }
  if (!WasapiCaptureAdapter::IsProcessLoopbackSupportedOnCurrentWindows()) {
    return 0;
  }
  return source_mode == AudioSourceMode::ApplicationProcessLoopback ? 2 : 3;
}

AudioSourceMode SourceModeFromComboIndex(int index) {
  if (index <= 0) {
    return AudioSourceMode::MicrophoneCapture;
  }
  if (index == 1) {
    return AudioSourceMode::SystemLoopback;
  }
  if (!WasapiCaptureAdapter::IsProcessLoopbackSupportedOnCurrentWindows()) {
    return AudioSourceMode::MicrophoneCapture;
  }
  return index == 2 ? AudioSourceMode::ApplicationProcessLoopback
                    : AudioSourceMode::ApplicationLoopback;
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

WasapiStreamCategory StreamCategoryFromComboIndex(int index) {
  switch (index) {
    case 0:
      return WasapiStreamCategory::Communications;
    case 1:
      return WasapiStreamCategory::Media;
    case 2:
      return WasapiStreamCategory::Movie;
    case 3:
      return WasapiStreamCategory::Speech;
    case 4:
      return WasapiStreamCategory::GameChat;
    case 5:
      return WasapiStreamCategory::GameMedia;
    case 6:
      return WasapiStreamCategory::GameEffects;
    case 7:
      return WasapiStreamCategory::SoundEffects;
    case 8:
      return WasapiStreamCategory::Alerts;
    case 9:
      return WasapiStreamCategory::Other;
    case 10:
      return WasapiStreamCategory::ForegroundOnlyMedia;
    case 11:
      return WasapiStreamCategory::BackgroundCapableMedia;
    case 12:
      return WasapiStreamCategory::FarFieldSpeech;
    case 13:
      return WasapiStreamCategory::UniformSpeech;
    case 14:
      return WasapiStreamCategory::VoiceTyping;
    default:
      return WasapiStreamCategory::Communications;
  }
}

int ComboIndexFromStreamCategory(WasapiStreamCategory category) {
  switch (category) {
    case WasapiStreamCategory::Communications:
      return 0;
    case WasapiStreamCategory::Media:
      return 1;
    case WasapiStreamCategory::Movie:
      return 2;
    case WasapiStreamCategory::Speech:
      return 3;
    case WasapiStreamCategory::GameChat:
      return 4;
    case WasapiStreamCategory::GameMedia:
      return 5;
    case WasapiStreamCategory::GameEffects:
      return 6;
    case WasapiStreamCategory::SoundEffects:
      return 7;
    case WasapiStreamCategory::Alerts:
      return 8;
    case WasapiStreamCategory::Other:
      return 9;
    case WasapiStreamCategory::ForegroundOnlyMedia:
      return 10;
    case WasapiStreamCategory::BackgroundCapableMedia:
      return 11;
    case WasapiStreamCategory::FarFieldSpeech:
      return 12;
    case WasapiStreamCategory::UniformSpeech:
      return 13;
    case WasapiStreamCategory::VoiceTyping:
      return 14;
  }
  return 0;
}

WasapiStreamOptions StreamOptionsFromComboIndex(int index) {
  switch (index) {
    case 0:
      return WasapiStreamOptions::Raw;
    case 1:
      return WasapiStreamOptions::None;
    case 2:
      return static_cast<WasapiStreamOptions>(
          static_cast<uint32_t>(WasapiStreamOptions::Raw) |
          static_cast<uint32_t>(WasapiStreamOptions::MatchFormat));
    case 3:
      return WasapiStreamOptions::MatchFormat;
    case 4:
      return WasapiStreamOptions::Ambisonics;
    case 5:
      return WasapiStreamOptions::PostVolumeLoopback;
    default:
      return WasapiStreamOptions::Raw;
  }
}

int ComboIndexFromStreamOptions(WasapiStreamOptions options) {
  if (options == WasapiStreamOptions::Raw) {
    return 0;
  }
  if (options == WasapiStreamOptions::None) {
    return 1;
  }
  if (options ==
      static_cast<WasapiStreamOptions>(
          static_cast<uint32_t>(WasapiStreamOptions::Raw) |
          static_cast<uint32_t>(WasapiStreamOptions::MatchFormat))) {
    return 2;
  }
  if (options == WasapiStreamOptions::MatchFormat) {
    return 3;
  }
  if (options == WasapiStreamOptions::Ambisonics) {
    return 4;
  }
  if (options == WasapiStreamOptions::PostVolumeLoopback) {
    return 5;
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
      config.capture.source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      config.capture.source_mode == AudioSourceMode::ApplicationLoopback;

  SendMessageW(context->capture_backend_combo, CB_SETCURSEL,
               config.capture.backend == AudioBackendType::Wasapi ? 0 : 1, 0);
  SendMessageW(context->render_backend_combo, CB_SETCURSEL,
               config.render.backend == AudioBackendType::Wasapi ? 0 : 1, 0);
  SendMessageW(context->source_mode_combo, CB_SETCURSEL,
               SourceModeToComboIndex(config.capture.source_mode), 0);
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
  SendMessageW(context->capture_category_combo, CB_SETCURSEL,
               ComboIndexFromStreamCategory(
                   config.capture.wasapi_stream_category),
               0);
  SendMessageW(context->capture_options_combo, CB_SETCURSEL,
               ComboIndexFromStreamOptions(
                   config.capture.wasapi_stream_options),
               0);
  SendMessageW(context->render_share_mode_combo, CB_SETCURSEL,
               config.render.wasapi_share_mode == WasapiShareMode::Shared ? 0 : 1,
               0);
  SendMessageW(context->render_drive_mode_combo, CB_SETCURSEL,
               config.render.wasapi_drive_mode == WasapiDriveMode::EventDriven ? 0
                                                                               : 1,
               0);
  SendMessageW(context->render_category_combo, CB_SETCURSEL,
               ComboIndexFromStreamCategory(
                   config.render.wasapi_stream_category),
               0);
  SendMessageW(context->render_options_combo, CB_SETCURSEL,
               ComboIndexFromStreamOptions(
                   config.render.wasapi_stream_options),
               0);
  SetControlText(context->dump_path_edit, config.capture.dump_path.c_str());
  SetControlText(context->app_loopback_process_edit,
                 config.capture.application_loopback_target_value.c_str());
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
  const auto rtc_stats = context->model.rtc_stats();
  SetControlText(context->rtc_join_leave_button,
                 BuildRtcJoinButtonLabelText(config.rtc, rtc_stats).c_str());
  SetControlText(context->rtc_status_label,
                 BuildRtcStatusLabelText(config, context->model.session_state(),
                                         rtc_stats).c_str());
  SetControlText(context->rtc_app_id_edit, config.rtc.app_id.c_str());
  SetControlText(context->rtc_channel_edit, config.rtc.channel_id.c_str());
  SetControlText(context->rtc_uid_edit, std::to_wstring(config.rtc.uid).c_str());
  SetControlText(context->rtc_token_edit, config.rtc.token.c_str());
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
  TextOutW(hdc, rect.left + 16, layout.toolbar_rect.top + 14, L"Session Controls", 16);
  TextOutW(hdc, layout.routing_rect.left + 16, layout.routing_rect.top + 10,
           L"Signal Routing", 14);
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
      source_mode == AudioSourceMode::ApplicationProcessLoopback ||
      source_mode == AudioSourceMode::ApplicationLoopback;
  const auto capture_device_label =
      source_mode == AudioSourceMode::ApplicationProcessLoopback
          ? std::wstring {L"Target Process ID"}
          : (source_mode == AudioSourceMode::ApplicationLoopback
                 ? std::wstring {L"Target Application (.exe)"}
                 : BuildCaptureDeviceLabelText(source_mode));
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
  TextOutW(hdc, layout.capture_format_rect.left + section_inset,
           layout.format_row3_label_y,
           L"Category", 8);
  TextOutW(hdc,
           layout.capture_format_rect.left + section_inset + layout.category_width +
               inner_gap,
           layout.format_row3_label_y, L"Options", 7);

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
  TextOutW(hdc, layout.render_format_rect.left + section_inset,
           layout.format_row3_label_y,
           L"Category", 8);
  TextOutW(hdc,
           layout.render_format_rect.left + section_inset + layout.category_width +
               inner_gap,
           layout.format_row3_label_y, L"Options", 7);

  TextOutW(hdc, layout.output_rect.left + section_inset, layout.output_label_y,
           L"Dump Path", 9);
  TextOutW(hdc,
           layout.output_left + layout.dump_path_width + layout.dump_checkbox_width +
               (inner_gap * 2),
           layout.output_label_y,
           L"Dump Type", 9);
  TextOutW(hdc, layout.output_rect.left + section_inset, layout.output_label_y2,
           L"Cap Buffer (ms)", 15);
  TextOutW(hdc,
           layout.output_rect.left + section_inset + layout.buffer_width + 12,
           layout.output_label_y2, L"Ren Buffer (ms)", 15);
  TextOutW(hdc, layout.output_rect.left + section_inset, layout.output_label_y3,
           L"Session Flags", 13);
  TextOutW(hdc, layout.output_right_left, layout.output_rect.top + 10,
           L"RTC Session", 11);
  TextOutW(hdc, layout.output_right_left, layout.rtc_row2_y - 14,
           L"App ID", 6);
  TextOutW(hdc, layout.output_right_left, layout.rtc_row3_y - 14,
           L"Channel", 7);
  TextOutW(hdc,
           layout.output_right_left + layout.output_right_width - layout.rtc_uid_width,
           layout.rtc_row3_y - 14, L"UID", 3);
  TextOutW(hdc, layout.output_right_left, layout.rtc_token_y - 14,
           L"Token", 5);
}

void DrawConfigLabels(HDC hdc, const RECT& client_rect, WindowContext* context) {
  if (context == nullptr) {
    return;
  }
  const AppLayout layout = CalculateAppLayout(client_rect);
  const auto source_mode = context->model.configuration().capture.source_mode;
  DrawConfigLabels(hdc, layout.config_rect, source_mode);
}

bool UpdateWindowTitle(HWND hwnd, WindowContext* context) {
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
  const bool changed = context->last_window_title != title;
  SetWindowTextIfChanged(hwnd, context->last_window_title, title);
  return changed;
}

void SyncWindowState(HWND hwnd, WindowContext* context) {
  if (context == nullptr) {
    return;
  }
  SyncAutomationTextMirrors(context);
  UpdateWindowTitle(hwnd, context);
  UpdateTimerState(hwnd, context);
}

void RefreshWindowFromModel(HWND hwnd,
                            WindowContext* context,
                            BOOL erase_background = TRUE) {
  SyncWindowState(hwnd, context);
  InvalidateRect(hwnd, nullptr, erase_background);
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

void DrawInfoTabsBackground(HDC hdc, const RECT& rect) {
  HBRUSH background = CreateSolidBrush(RGB(245, 247, 250));
  FillRect(hdc, &rect, background);
  DeleteObject(background);
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
      owned_context->capture_open_probe_button = CreateWindowW(
          L"BUTTON", L"Find Capture Params", WS_TABSTOP | WS_VISIBLE | WS_CHILD,
          706, 16, 150, 28, hwnd, ControlIdToMenu(kButtonCaptureOpenProbe),
          nullptr, nullptr);
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
      owned_context->capture_category_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 760, 206,
          130, 220, hwnd, ControlIdToMenu(kComboCaptureCategory), nullptr,
          nullptr);
      owned_context->capture_options_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 920, 206,
          130, 220, hwnd, ControlIdToMenu(kComboCaptureOptions), nullptr,
          nullptr);
      owned_context->render_share_mode_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 760, 226,
          130, 180, hwnd, ControlIdToMenu(kComboRenderShareMode), nullptr,
          nullptr);
      owned_context->render_drive_mode_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 920, 226,
          130, 180, hwnd, ControlIdToMenu(kComboRenderDriveMode), nullptr,
          nullptr);
      owned_context->render_category_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 760, 256,
          130, 220, hwnd, ControlIdToMenu(kComboRenderCategory), nullptr,
          nullptr);
      owned_context->render_options_combo = CreateWindowW(
          L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST, 920, 256,
          130, 220, hwnd, ControlIdToMenu(kComboRenderOptions), nullptr,
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
      owned_context->rtc_join_leave_button = CreateWindowW(
          L"BUTTON", L"JoinChannel", WS_TABSTOP | WS_CHILD | WS_VISIBLE, 700,
          386, 132, 28, hwnd, ControlIdToMenu(kButtonRtcJoinLeave), nullptr,
          nullptr);
      owned_context->rtc_status_label = CreateWindowW(
          L"STATIC", L"Status: Not joined", WS_CHILD | WS_VISIBLE, 844, 390,
          220, 20, hwnd, ControlIdToMenu(kStaticRtcStatus), nullptr, nullptr);
      owned_context->rtc_app_id_edit = CreateWindowW(
          L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 700,
          446, 200, 24, hwnd, ControlIdToMenu(kEditRtcAppId), nullptr, nullptr);
      owned_context->rtc_channel_edit = CreateWindowW(
          L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 700,
          476, 140, 24, hwnd, ControlIdToMenu(kEditRtcChannel), nullptr, nullptr);
      owned_context->rtc_uid_edit = CreateWindowW(
          L"EDIT", L"0", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 850,
          476, 60, 24, hwnd, ControlIdToMenu(kEditRtcUid), nullptr, nullptr);
      owned_context->rtc_token_edit = CreateWindowW(
          L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 700,
          506, 200, 24, hwnd, ControlIdToMenu(kEditRtcToken), nullptr, nullptr);
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
      owned_context->info_tabs = CreateWindowExW(
          0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
          0, 0, 0, 0, hwnd, ControlIdToMenu(kTabInfoPages), nullptr, nullptr);
      owned_context->overview_snapshot_label = CreateWindowW(
          L"STATIC", L"Session Snapshot", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
          ControlIdToMenu(kOverviewSnapshotLabel), nullptr, nullptr);
      owned_context->overview_summary_label = CreateWindowW(
          L"STATIC", L"Configured Summary", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
          ControlIdToMenu(kOverviewSummaryLabel), nullptr, nullptr);
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
      owned_context->logs_console_toggle_button = CreateWindowW(
          L"BUTTON", L"Open Console", WS_TABSTOP | WS_CHILD, 0, 0, 0, 0, hwnd,
          ControlIdToMenu(kButtonLogsConsoleToggle), nullptr, nullptr);
      owned_context->visible_recent_logs_text = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
              WS_VSCROLL,
          0, 0, 0, 0, hwnd, ControlIdToMenu(kVisibleRecentLogsText), nullptr,
          nullptr);
      owned_context->visible_rtc_text = CreateWindowExW(
          WS_EX_CLIENTEDGE, L"EDIT", L"",
          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
              WS_VSCROLL,
          0, 0, 0, 0, hwnd, ControlIdToMenu(kVisibleRtcText), nullptr,
          nullptr);
      owned_context->panel_text_font = CreatePanelTextFont();
      owned_context->panel_mono_font = CreatePanelMonoFont();
      owned_context->panel_header_font = CreatePanelHeaderFont();
      owned_context->panel_edit_brush = CreateSolidBrush(RGB(252, 253, 255));
      owned_context->panel_diagnostic_brush = CreateSolidBrush(RGB(248, 251, 255));
      ConfigureReadOnlyPanelEdit(owned_context->visible_snapshot_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_summary_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_diagnostics_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_rtc_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_capability_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_probe_text);
      ConfigureReadOnlyPanelEdit(owned_context->visible_recent_logs_text);
      const HWND combo_controls[] = {
          owned_context->capture_backend_combo,
          owned_context->render_backend_combo,
          owned_context->source_mode_combo,
          owned_context->capture_device_combo,
          owned_context->render_device_combo,
          owned_context->capture_sample_rate_combo,
          owned_context->capture_channels_combo,
          owned_context->capture_sample_type_combo,
          owned_context->render_sample_rate_combo,
          owned_context->render_channels_combo,
          owned_context->render_sample_type_combo,
          owned_context->capture_share_mode_combo,
          owned_context->capture_drive_mode_combo,
          owned_context->capture_category_combo,
          owned_context->capture_options_combo,
          owned_context->render_share_mode_combo,
          owned_context->render_drive_mode_combo,
          owned_context->render_category_combo,
          owned_context->render_options_combo,
          owned_context->dump_type_combo,
      };
      for (HWND combo : combo_controls) {
        ConfigureComboDropdown(combo);
      }
      TCITEMW tab_item {};
      tab_item.mask = TCIF_TEXT;
      wchar_t overview_text[] = L"Overview";
      tab_item.pszText = overview_text;
      SendMessageW(owned_context->info_tabs, TCM_INSERTITEMW, 0,
                   reinterpret_cast<LPARAM>(&tab_item));
      wchar_t diagnostics_text[] = L"Diagnostics";
      tab_item.pszText = diagnostics_text;
      SendMessageW(owned_context->info_tabs, TCM_INSERTITEMW, 1,
                   reinterpret_cast<LPARAM>(&tab_item));
      wchar_t rtc_text[] = L"RTC";
      tab_item.pszText = rtc_text;
      SendMessageW(owned_context->info_tabs, TCM_INSERTITEMW, 2,
                   reinterpret_cast<LPARAM>(&tab_item));
      wchar_t capabilities_text[] = L"Capabilities";
      tab_item.pszText = capabilities_text;
      SendMessageW(owned_context->info_tabs, TCM_INSERTITEMW, 3,
                   reinterpret_cast<LPARAM>(&tab_item));
      wchar_t probe_text[] = L"Probe";
      tab_item.pszText = probe_text;
      SendMessageW(owned_context->info_tabs, TCM_INSERTITEMW, 4,
                   reinterpret_cast<LPARAM>(&tab_item));
      wchar_t logs_text[] = L"Logs";
      tab_item.pszText = logs_text;
      SendMessageW(owned_context->info_tabs, TCM_INSERTITEMW, 5,
                   reinterpret_cast<LPARAM>(&tab_item));
      TabCtrl_SetCurSel(owned_context->info_tabs, 0);
      ApplyPanelTextFont(owned_context->visible_snapshot_text, owned_context->panel_text_font);
      ApplyPanelTextFont(owned_context->visible_summary_text, owned_context->panel_text_font);
      ApplyPanelTextFont(owned_context->visible_diagnostics_text, owned_context->panel_mono_font);
      ApplyPanelTextFont(owned_context->visible_rtc_text, owned_context->panel_mono_font);
      ApplyPanelTextFont(owned_context->visible_capability_text, owned_context->panel_mono_font);
      ApplyPanelTextFont(owned_context->visible_probe_text, owned_context->panel_mono_font);
      ApplyPanelTextFont(owned_context->visible_recent_logs_text, owned_context->panel_text_font);
      ApplyPanelTextFont(owned_context->logs_console_toggle_button,
                         owned_context->panel_text_font);
      ApplyPanelTextFont(owned_context->overview_snapshot_label,
                         owned_context->panel_header_font);
      ApplyPanelTextFont(owned_context->overview_summary_label,
                         owned_context->panel_header_font);

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
      PopulateSimpleCombo(owned_context->capture_category_combo,
                          {L"Communications", L"Media", L"Movie", L"Speech",
                           L"Game Chat", L"Game Media", L"Game Effects",
                           L"Sound Effects", L"Alerts", L"Other",
                           L"Foreground Media", L"Background Media",
                           L"Far Field Speech", L"Uniform Speech",
                           L"Voice Typing"});
      PopulateSimpleCombo(owned_context->capture_options_combo,
                          {L"Raw", L"None", L"Raw | MatchFormat",
                           L"MatchFormat", L"Ambisonics",
                           L"PostVolumeLoopback"});
      PopulateSimpleCombo(owned_context->render_share_mode_combo,
                          {L"Shared", L"Exclusive"});
      PopulateSimpleCombo(owned_context->render_drive_mode_combo,
                          {L"Event", L"Timer"});
      PopulateSimpleCombo(owned_context->render_category_combo,
                          {L"Communications", L"Media", L"Movie", L"Speech",
                           L"Game Chat", L"Game Media", L"Game Effects",
                           L"Sound Effects", L"Alerts", L"Other",
                           L"Foreground Media", L"Background Media",
                           L"Far Field Speech", L"Uniform Speech",
                           L"Voice Typing"});
      PopulateSimpleCombo(owned_context->render_options_combo,
                          {L"Raw", L"None", L"Raw | MatchFormat",
                           L"MatchFormat", L"Ambisonics",
                           L"PostVolumeLoopback"});
      PopulateSimpleCombo(owned_context->dump_type_combo, {L"WAV", L"PCM"});
      SyncUiFromModel(owned_context);
      LayoutChildControls(hwnd, owned_context);
      owned_context->device_notifications = new DeviceNotificationClient(hwnd);
      owned_context->device_notifications->Register();
      UpdateWindowTitle(hwnd, owned_context);
      UpdateTimerState(hwnd, owned_context);
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
            edit == context->visible_rtc_text ||
            edit == context->visible_capability_text ||
            edit == context->visible_probe_text;
        if (is_visible_panel) {
          auto hdc = reinterpret_cast<HDC>(w_param);
          const bool is_diagnostic_panel =
              edit == context->visible_diagnostics_text ||
              edit == context->visible_rtc_text ||
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
          RefreshWindowFromModel(hwnd, context);
          return 0;
        case kButtonStop:
          context->model.Stop();
          ApplyControlAvailability(context);
          RefreshWindowFromModel(hwnd, context);
          return 0;
        case kButtonRefresh:
          context->model.RefreshDevices(true);
          SyncUiFromModel(context);
          RefreshWindowFromModel(hwnd, context);
          return 0;
        case kButtonProbe:
          if (!context->probe_running) {
            context->probe_mode = ProbeUiMode::Quick;
            SetProbeUiBusy(context, true);
            SyncWindowState(hwnd, context);
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
            SyncWindowState(hwnd, context);
            context->probe_thread = std::jthread([hwnd, context]() {
              context->model.RunProbeMatrix();
              PostMessageW(hwnd, kMessageProbeMatrixFinished, 0, 0);
            });
          }
          return 0;
        case kButtonCaptureOpenProbe:
          if (!context->probe_running) {
            context->probe_mode = ProbeUiMode::CaptureOpen;
            SetProbeUiBusy(context, true);
            SyncWindowState(hwnd, context);
            context->probe_thread = std::jthread([hwnd, context]() {
              context->model.RunCaptureOpenProbe();
              PostMessageW(hwnd, kMessageProbeFinished, 0, 0);
            });
          }
          return 0;
        case kButtonLogsConsoleToggle:
          if (context->logs_console_open) {
            CloseLogsConsole(context);
          } else if (OpenLogsConsole(context, hwnd)) {
            WriteRecentLogsToConsole(context, context->model.log_snapshot());
          }
          SyncWindowState(hwnd, context);
          return 0;
        case kComboCaptureBackend:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_backend_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureBackend(index == 0 ? AudioBackendType::Wasapi
                                                        : AudioBackendType::WaveApi);
            SyncUiFromModel(context);
            RefreshWindowFromModel(hwnd, context);
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
            RefreshWindowFromModel(hwnd, context);
          }
          return 0;
        case kComboSourceMode:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->source_mode_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureSourceMode(SourceModeFromComboIndex(index));
            SyncUiFromModel(context);
            LayoutChildControls(hwnd, context);
            RefreshWindowFromModel(hwnd, context);
          }
          return 0;
        case kEditAppLoopbackProcess:
          if (HIWORD(w_param) == EN_CHANGE) {
            const auto source_mode = context->model.configuration().capture.source_mode;
            context->model.SetApplicationLoopbackTarget(
                source_mode == AudioSourceMode::ApplicationProcessLoopback
                    ? ApplicationLoopbackTargetKind::ProcessId
                    : ApplicationLoopbackTargetKind::ApplicationName,
                GetWindowTextString(context->app_loopback_process_edit));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboCaptureDevice:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            context->model.SetCaptureDeviceId(
                GetSelectedComboDeviceId(context->capture_device_combo));
            RefreshWindowFromModel(hwnd, context);
          }
          return 0;
        case kComboRenderDevice:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            context->model.SetRenderDeviceId(
                GetSelectedComboDeviceId(context->render_device_combo));
            RefreshWindowFromModel(hwnd, context);
          }
          return 0;
        case kEditDelayMs:
          if (HIWORD(w_param) == EN_CHANGE) {
            const auto text = GetWindowTextString(context->delay_edit);
            if (!text.empty()) {
              context->model.SetFixedDelayMs(static_cast<uint32_t>(std::wcstoul(
                  text.c_str(), nullptr, 10)));
              SyncWindowState(hwnd, context);
            }
          }
          return 0;
        case kCheckboxDump:
          context->model.SetDumpEnabled(
              SendMessageW(context->dump_checkbox, BM_GETCHECK, 0, 0) ==
              BST_CHECKED);
          SyncWindowState(hwnd, context);
          return 0;
        case kComboCaptureSampleRate:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_sample_rate_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureSampleRate(SampleRateFromComboIndex(index));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboCaptureChannels:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_channels_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureChannels(ChannelsFromComboIndex(index));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboCaptureSampleType:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_sample_type_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureSampleType(SampleTypeFromComboIndex(index));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboRenderSampleRate:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_sample_rate_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderSampleRate(SampleRateFromComboIndex(index));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboRenderChannels:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_channels_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderChannels(ChannelsFromComboIndex(index));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboRenderSampleType:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_sample_type_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderSampleType(SampleTypeFromComboIndex(index));
            SyncWindowState(hwnd, context);
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
            SyncWindowState(hwnd, context);
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
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboCaptureCategory:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_category_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureWasapiStreamCategory(
                StreamCategoryFromComboIndex(index));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboCaptureOptions:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->capture_options_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetCaptureWasapiStreamOptions(
                StreamOptionsFromComboIndex(index));
            SyncWindowState(hwnd, context);
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
            SyncWindowState(hwnd, context);
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
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboRenderCategory:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_category_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderWasapiStreamCategory(
                StreamCategoryFromComboIndex(index));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboRenderOptions:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->render_options_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetRenderWasapiStreamOptions(
                StreamOptionsFromComboIndex(index));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kEditDumpPath:
          if (HIWORD(w_param) == EN_CHANGE) {
            context->model.SetDumpPath(
                GetWindowTextString(context->dump_path_edit));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kComboDumpType:
          if (HIWORD(w_param) == CBN_SELCHANGE) {
            const auto index =
                static_cast<int>(SendMessageW(context->dump_type_combo,
                                              CB_GETCURSEL, 0, 0));
            context->model.SetDumpFileType(index == 0 ? DumpFileType::Wav
                                                      : DumpFileType::RawPcm);
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kEditCaptureBufferMs:
          if (HIWORD(w_param) == EN_CHANGE) {
            const auto text = GetWindowTextString(context->capture_buffer_edit);
            if (!text.empty()) {
              context->model.SetCaptureBufferDurationMs(static_cast<uint32_t>(
                  std::wcstoul(text.c_str(), nullptr, 10)));
              SyncWindowState(hwnd, context);
            }
          }
          return 0;
        case kEditRenderBufferMs:
          if (HIWORD(w_param) == EN_CHANGE) {
            const auto text = GetWindowTextString(context->render_buffer_edit);
            if (!text.empty()) {
              context->model.SetRenderBufferDurationMs(static_cast<uint32_t>(
                  std::wcstoul(text.c_str(), nullptr, 10)));
              SyncWindowState(hwnd, context);
            }
          }
          return 0;
        case kCheckboxMonitor:
          context->model.SetMonitorEnabled(
              SendMessageW(context->monitor_checkbox, BM_GETCHECK, 0, 0) ==
              BST_CHECKED);
          SyncUiFromModel(context);
          RefreshWindowFromModel(hwnd, context);
          return 0;
        case kCheckboxFollowDefaults:
          context->model.SetFollowDefaultDevices(
              SendMessageW(context->follow_defaults_checkbox, BM_GETCHECK, 0, 0) ==
              BST_CHECKED);
          SyncUiFromModel(context);
          RefreshWindowFromModel(hwnd, context);
          return 0;
        case kCheckboxAutoAlignRender:
          context->model.SetAutoAlignRenderFormat(
              SendMessageW(context->auto_align_render_checkbox, BM_GETCHECK, 0, 0) ==
              BST_CHECKED);
          SyncUiFromModel(context);
          RefreshWindowFromModel(hwnd, context);
          return 0;
        case kButtonRtcJoinLeave: {
          const auto config = context->model.configuration();
          const auto rtc_stats = context->model.rtc_stats();
          if (config.rtc.enabled || rtc_stats.joined) {
            context->model.LeaveRtcChannel();
          } else {
            context->model.JoinRtcChannel();
          }
          SyncUiFromModel(context);
          RefreshWindowFromModel(hwnd, context);
          return 0;
        }
        case kEditRtcAppId:
          if (HIWORD(w_param) == EN_CHANGE) {
            context->model.SetRtcAppId(GetWindowTextString(context->rtc_app_id_edit));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kEditRtcChannel:
          if (HIWORD(w_param) == EN_CHANGE) {
            context->model.SetRtcChannelId(
                GetWindowTextString(context->rtc_channel_edit));
            SyncWindowState(hwnd, context);
          }
          return 0;
        case kEditRtcUid:
          if (HIWORD(w_param) == EN_CHANGE) {
            const auto text = GetWindowTextString(context->rtc_uid_edit);
            if (!text.empty()) {
              context->model.SetRtcUid(
                  static_cast<uint32_t>(std::wcstoul(text.c_str(), nullptr, 10)));
              SyncWindowState(hwnd, context);
            }
          }
          return 0;
        case kEditRtcToken:
          if (HIWORD(w_param) == EN_CHANGE) {
            context->model.SetRtcToken(GetWindowTextString(context->rtc_token_edit));
            SyncWindowState(hwnd, context);
          }
          return 0;
        default:
          break;
      }
      break;
    }

    case WM_NOTIFY:
      if (context != nullptr) {
        auto* header = reinterpret_cast<LPNMHDR>(l_param);
        if (header != nullptr && header->hwndFrom == context->info_tabs &&
            header->code == TCN_SELCHANGE) {
          LayoutChildControls(hwnd, context);
          InvalidateRect(hwnd, nullptr, TRUE);
          return 0;
        }
      }
      break;

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
        const bool mirrors_changed = SyncAutomationTextMirrors(context);
        const bool title_changed = UpdateWindowTitle(hwnd, context);
        if (mirrors_changed || title_changed || context->probe_running ||
            IsSessionActive(context) || context->logs_console_open) {
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      }
      break;

    case WM_APP_DEVICE_CHANGE:
      if (context != nullptr) {
        context->model.HandleDefaultDeviceRefresh();
        SyncUiFromModel(context);
        RefreshWindowFromModel(hwnd, context);
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
          RefreshWindowFromModel(hwnd, context);
        }
      }
      return 0;

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      RECT client_rect {};
      GetClientRect(hwnd, &client_rect);

      const int paint_width = std::max(1, RectWidth(client_rect));
      const int paint_height = std::max(1, RectHeight(client_rect));
      HDC memory_dc = CreateCompatibleDC(hdc);
      HBITMAP buffer_bitmap =
          memory_dc != nullptr ? CreateCompatibleBitmap(hdc, paint_width, paint_height)
                               : nullptr;
      HGDIOBJ old_bitmap =
          (memory_dc != nullptr && buffer_bitmap != nullptr)
              ? SelectObject(memory_dc, buffer_bitmap)
              : nullptr;
      HDC target_dc =
          (memory_dc != nullptr && buffer_bitmap != nullptr) ? memory_dc : hdc;
      HBRUSH window_background = CreateSolidBrush(RGB(255, 255, 255));
      FillRect(target_dc, &client_rect, window_background);
      DeleteObject(window_background);

      if (context != nullptr) {
        const AppLayout layout = CalculateAppLayout(client_rect);
        DrawInfoTabsBackground(target_dc, layout.info_tabs_rect);
        DrawConfigLabels(target_dc, client_rect, context);

        WaveformRenderer::Draw(target_dc, layout.capture_waveform_rect,
                               context->model.capture_waveform(),
                               RGB(41, 182, 246), L"Capture Waveform",
                               context->model.stats().capture_meter);
        WaveformRenderer::Draw(target_dc, layout.render_waveform_rect,
                               context->model.render_waveform(),
                               RGB(255, 112, 67), L"Render Waveform",
                               context->model.stats().render_meter);
      }

      if (target_dc == memory_dc) {
        BitBlt(hdc, 0, 0, paint_width, paint_height, memory_dc, 0, 0, SRCCOPY);
      }
      if (memory_dc != nullptr && old_bitmap != nullptr) {
        SelectObject(memory_dc, old_bitmap);
      }
      if (buffer_bitmap != nullptr) {
        DeleteObject(buffer_bitmap);
      }
      if (memory_dc != nullptr) {
        DeleteDC(memory_dc);
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
        CloseLogsConsole(context);
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

  INITCOMMONCONTROLSEX common_controls {};
  common_controls.dwSize = sizeof(common_controls);
  common_controls.dwICC = ICC_TAB_CLASSES;
  InitCommonControlsEx(&common_controls);

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
  window_class.hbrBackground = nullptr;

  RegisterClassW(&window_class);

  HWND hwnd = CreateWindowExW(
      0, kWindowClassName, L"WinAudio Demo",
      WS_OVERLAPPEDWINDOW | WS_VISIBLE | WS_CLIPCHILDREN,
      CW_USEDEFAULT, CW_USEDEFAULT, 1280, 1120, nullptr, nullptr, instance,
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
