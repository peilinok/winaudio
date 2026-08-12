# WinAudio

Windows audio test tool: capture, loopback, delayed monitor pass-through, and chart visualization.

## Language

### Capture and monitor

**Monitor**:
A dual-stream session that captures audio and optionally plays it back after a fixed delay, with scope taps for charts.
_Avoid_: Engine session (when meaning the product feature), pass-through alone

**System Loopback**:
Capture of mixed system (render-device) audio rather than a microphone endpoint.
_Avoid_: Stereo mix (vendor-specific), what-u-hear

**Application Loopback**:
Capture of one process tree’s audio by PID (include or exclude tree modes).
_Avoid_: App capture, process record

**Scope tap**:
A visualization-only ring of recent samples produced beside the audio path; reading it must not block the pump.
_Avoid_: Ring buffer (when meaning the viz tap specifically), waveform buffer alone

### Charts and analysis

**Chart Data Pipeline**:
Per-frame refresh of chart sample history and spectrogram columns from scope taps, including freeze gating and hop-aligned spectrum advance. Does not own layout or ImGui drawing.
_Avoid_: drawComboPanel logic, analysis pump, FFT thread

**Scope Reader**:
The narrow read surface the Chart Data Pipeline uses to pull windows from scope taps (written count and ending-at / latest snapshots). Production adapter sits on the monitor session; tests use a fake.
_Avoid_: MonitorEngine (when only the read surface is meant), ScopeBuffer (storage primitive)

**Hop**:
Fixed sample-count step between successive analysis windows that feed one spectrogram column.
_Avoid_: Frame (GUI frame), buffer period alone

**Chart freeze**:
User-visible pause of chart data refresh only; capture and playback continue.
_Avoid_: Pause (when meaning stop the session), Stop

**Linked time axis**:
Shared horizontal time window across waveforms and spectrograms on one page.
_Avoid_: x-zoom alone, history length (buffer capacity)

**Chart Host**:
The right-hand charts column on a page: ensure visual buffers, freeze/zoom toolbar, draw the active chart panels, and clear one-shot view resets. Does not own the left control panel or the log region.
_Avoid_: whole page layout, Chart Data Pipeline, drawComboPanel alone

**Dual reorderable host**:
Chart Host mode with capture and render combos and user-reorderable panel order (Monitor).
_Avoid_: dual column layout (when meaning only UI chrome)

**Capture-only host**:
Chart Host mode with a single capture combo and no render charts (System Loopback and Application Loopback).
_Avoid_: mono monitor (when meaning audio channels)

### Streams

**Capture side**:
The input leg of a monitor or loopback session (endpoint, system loopback, or application loopback).
_Avoid_: Record side, mic only

**Render side**:
The delayed playback leg of a monitor session when sync playback is engaged.
_Avoid_: Output only, speaker path alone
