# WinAudio System Loopback Design

> **历史设计。** 当前多路 Capture Track 的生命周期、每路 silent render 与页面行为见 [多路回采设计](2026-08-14-winaudio-multi-track-loopback-design.md)。下文仅保留 WASAPI 机制背景；一页一个 MonitorEngine 的旧布局已过时。

## Scope

This phase adds system loopback capture only. Application/process loopback stays
out of scope because it uses a different activation path
(`ActivateAudioInterfaceAsync` with a process-loopback virtual device) and should
be a separate backend rather than a `WasapiStream` subclass.

System loopback captures a render endpoint with `IAudioCaptureClient` and
`AUDCLNT_STREAMFLAGS_LOOPBACK`. It is shared-mode only. The app does not expose a
fake capture device; render endpoints remain render endpoints.

## WASAPI Facts and Risks

System loopback is tied to the selected render device. Empty device ids resolve
to the default render endpoint in the WASAPI open path.

Event-driven loopback was verified on the current Windows target with active
playback: a float32 tone produced non-zero loopback capture and the wait path
woke from events rather than the timeout fallback. Idle/no-playback behavior is
OS dependent: loopback may deliver silent packets or may have no packets at all.
The UI treats no-packet gaps as a frozen capture scope, not generated silence.

Requested shared formats are passed through the normal shared initialization
path. Failures surface as normal WASAPI Initialize errors.

## Core Architecture

`CaptureSource` distinguishes endpoint capture from system loopback:

- `Endpoint`: device id is a capture endpoint.
- `SystemLoopback`: device id is a render endpoint.

`WasapiSystemLoopbackCaptureStream` reuses the capture service and ring path,
but overrides WASAPI flow to render and adds the loopback stream flag. Exclusive
mode is rejected from `open()` with `Result::Fail`.

`MonitorEngine` accepts `CaptureSource` while retaining the legacy `capId`
overload. The backend factory seam receives the source for capture creation so
tests can assert source plumbing. Feedback protection lives in core: loopback
capture with playback enabled is rejected when source and output are the same
render endpoint, including the runtime `setPlaybackEnabled(true)` path.

## GUI

The GUI has a top-level Loopback page. It uses a separate `MonitorEngine`
instance in capture-only mode:

```text
CaptureSource{SystemLoopback, renderId}, playbackEnabled=false
```

The page reuses the existing waveform/spectrogram drawing and MonitorEngine
snapshot plumbing through a parameterized visual state. Monitor and Loopback
state are separate; shutdown stops both engines.

## Verification

Automated coverage includes loopback source representation, exclusive rejection,
factory source plumbing, feedback rejection at start, and feedback rejection when
playback is enabled at runtime.

Manual or environment-dependent checks:

- Real system audio capture with active playback.
- Idle/no-playback visualization behavior on target OS builds.
- GUI click-through of the Loopback page Start/Stop path. In the current runner,
  `PrintWindow` can capture the UI, but synthetic mouse messages do not reliably
  drive Dear ImGui tab selection.
