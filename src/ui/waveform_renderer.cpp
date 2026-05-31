#include "waveform_renderer.h"

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

#include <algorithm>
#include <string>

namespace winaudio {

void WaveformRenderer::Draw(HDC hdc,
                            const RECT& rect,
                            const std::vector<WaveformEnvelopePoint>& waveform,
                            COLORREF accent_color,
                            const wchar_t* label,
                            const MeterValues& meter) {
  HBRUSH background = CreateSolidBrush(RGB(19, 24, 33));
  FillRect(hdc, &rect, background);
  DeleteObject(background);

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, RGB(224, 231, 239));
  TextOutW(hdc, rect.left + 8, rect.top + 6, label,
           static_cast<int>(wcslen(label)));

  const std::wstring meter_text =
      L"Peak " + std::to_wstring(meter.peak).substr(0, 4) + L"  RMS " +
      std::to_wstring(meter.rms).substr(0, 4) + L"  dBFS " +
      std::to_wstring(meter.dbfs).substr(0, 6);
  TextOutW(hdc, rect.left + 8, rect.top + 24, meter_text.c_str(),
           static_cast<int>(meter_text.size()));

  if (waveform.empty()) {
    return;
  }

  HPEN pen = CreatePen(PS_SOLID, 1, accent_color);
  HGDIOBJ old_pen = SelectObject(hdc, pen);

  const int center_y = (rect.top + rect.bottom) / 2 + 12;
  const int height = std::max(1L, (rect.bottom - rect.top - 40L) / 2L);
  const int width = std::max(1L, rect.right - rect.left - 16L);
  const size_t points = waveform.size();

  for (size_t index = 0; index < points; ++index) {
    const auto& point = waveform[index];
    const int x = rect.left + 8 +
                  static_cast<int>(index * width / std::max<size_t>(1, points - 1));
    const int y1 = center_y - static_cast<int>(point.max_value * height);
    const int y2 = center_y - static_cast<int>(point.min_value * height);
    MoveToEx(hdc, x, y1, nullptr);
    LineTo(hdc, x, y2);
  }

  SelectObject(hdc, old_pen);
  DeleteObject(pen);
}

}  // namespace winaudio
