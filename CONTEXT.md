# WinAudio

Windows audio test tool: capture, loopback, delayed monitor pass-through, and chart visualization.

## Language

### Capture and monitor

**Monitor**:
A dual-stream session that pairs exactly one Capture Track with at most one Render Track after a fixed delay, with scope taps for charts. Each side may own a live WAV sink (GUI Dump capture / Dump render).
_Avoid_: Engine session (when meaning the product feature), pass-through alone, Track (when meaning the whole pair)

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
User-visible pause of chart data refresh only; capture and playback continue. On a loopback page it applies to one Track’s Chart Host, not the whole page.
_Avoid_: Pause (when meaning stop the session), Stop

**Linked time axis**:
Shared horizontal time window across waveforms and spectrograms of one Track (including that Track’s channels). Loopback pages do not share one axis across Tracks. The Monitor page still links the pair.
_Avoid_: x-zoom alone, history length (buffer capacity), page-wide axis (when several Tracks are shown)

**Chart Host**:
Chart chrome for one visualization unit: freeze/zoom toolbar and the active panels. A loopback page stacks one Capture-only host per Capture Track. Does not own the left control panel or the log region.
_Avoid_: whole page layout, Chart Data Pipeline, drawComboPanel alone

**Dual reorderable host**:
Chart Host mode with capture and render combos and user-reorderable panel order (Monitor).
_Avoid_: dual column layout (when meaning only UI chrome)

**Capture-only host**:
Chart Host mode with a single capture combo and no render charts (System Loopback and Application Loopback).
_Avoid_: mono monitor (when meaning audio channels)

### Streams

**Track**:
A unidirectional stream the user can create and destroy on its own: either a capture of one source or a playback to one render endpoint. Create starts it immediately; there is no idle Track. It owns that source or endpoint, its own scope tap, and its own lifetime and status.
_Avoid_: session (when meaning one direction), channel, mix, page, Monitor (when meaning one direction)

**Capture source**:
The kind and identity of what a Capture Track captures: a capture endpoint, a System Loopback render endpoint, or an Application Loopback process tree. It is the recipe, not the running instance.
_Avoid_: Track, device (when meaning the whole instance)

**Capture Track**:
A Track that binds one Capture source and may own a WAV sink (GUI: live start/stop Dump; CLI capture: required path at create).
_Avoid_: record track, input track, Capture side (when meaning the Track itself)

**Render Track**:
A Track that plays to one render endpoint.
_Avoid_: playback track, output track, silent render (helper keepalive is not a Track)

**Capture side**:
The input leg of a Monitor — exactly one Capture Track — or a Capture Track on a loopback page.
_Avoid_: Record side, mic only

**Render side**:
The delayed playback leg of a Monitor when sync playback is engaged — at most one Render Track.
_Avoid_: Output only, speaker path alone

**Stream params**:
Advanced open-time settings for one Track: category, option, offload, ducking, buffer length, and user-overridable stream flags including AutoConvert (`Default` | `Force` | `Off`). All-default means follow the system and the current Stream init recipe.
_Avoid_: StreamOption (that is only the APO / raw / match-format field), client properties alone, AUDCLNT_STREAMFLAGS

**Stream flags**:
Initialize flags consumed by Stream init. User-overridable flags live in stream params (AutoConvert). Event-driven callback is locked on. Loopback extras stay caller-supplied, not in stream params.
_Avoid_: StreamOption, AUDCLNT_STREAMOPTIONS, extraSharedInitFlags

**Stream init**:
The share-mode recipe shared by every Track and by silent-render: mix vs requested conversion, exclusive probe and align-retry, duration, and stream flags. Loopback extras are supplied by the caller; Stream init does not inspect the Capture source. Endpoint or Application Loopback activation sits outside it, as do client properties, ducking, and the pump.
_Avoid_: Client Init, prepareClient, format negotiation (that is only the format half)

### Pipeline inspector

**Pipeline Inspector**:
A reconstruction of one capture or render path (hardware mic → app, or app → hardware), from a Live session plus optional Hooked streams, the endpoint graph, Shared probes, and ETW Initialize hints. It intercepts Core Audio COM in the target app process; it does not intercept APO COM in audiodg.
_Avoid_: APO dumper, API Monitor clone, session APO list

**Live session**:
A Windows audio session on one endpoint for one process — the mixer row used to discover who is capturing or playing. It is not a Track, not a Monitor, and may contain more than one Hooked stream.
_Avoid_: session (when meaning a Track or Monitor), process (too coarse), WASAPI stream

**Hooked stream**:
One `IAudioClient` in a target process whose Core Audio COM calls are intercepted in-process. Control-path methods carry full arguments; buffer pump calls are metadata only. It is not an audiodg APO object.
_Avoid_: Live session (the mixer row), attach as caller (we do not `GetService` their client from our process)

**Core Audio intercept**:
The closed COM set captured on a Hooked stream: device activate, `IAudioClient*`, capture/render clients (metadata only), `IAudioEffectsManager`, session control, stream/channel volume, and audio clock. Not XAudio2, DirectSound, or WinRT AudioGraph entry points, and not audiodg APO COM.
_Avoid_: full Win32 hook table, APO intercept

**On-demand attach**:
Injection into the process PID of a Live session the user selected, only when GUI and target have the same bitness. Cross-bitness attach fails closed. The GUI stays unelevated; attach that lacks debug rights fails closed and the rest of the Inspector still runs. WinAudio does not inject every audio process and does not launch the target suspended.
_Avoid_: global inject, monitor-new-process, attach as caller, always-admin GUI, 32-bit helper from x64 GUI

**Call log**:
The time-ordered list of intercepted Core Audio COM calls on Hooked streams: interface, method, arguments, HRESULT. Control-path calls are kept. Buffer pump metadata is off until enabled; when on, it uses a small ring (recent calls plus xrun counts) and never includes PCM. It sits beside the pipeline graph; it is not the graph.
_Avoid_: API Monitor dump, PCM capture, ETW etl file, unbounded GetBuffer log

**Observation kind**:
The confidence label on a pipeline node or parameter: Observed, Probed, Inferred, Skipped, or Unknown. Hooked-stream arguments are Observed. Shared-probe effect types stay Probed and must not be shown as the target's APO instances.
_Avoid_: guaranteed, actual APO instance, "what Chrome ran"

**Shared probe**:
A short-lived WASAPI Shared stream this process opens on the same endpoint as a Live session, to query advertised effect types when no Hooked stream is attached. Not Exclusive; not a clone of the target's stream.
_Avoid_: Exclusive probe, Hooked stream

**AudioClientInitialize (ETW)**:
Initialize-related events used to fill Initialize fields for a Live session before On-demand attach, or when attach is absent. Match by PID + short time window + device id, or else Unknown. After attach, Hooked-stream calls override these fields.
_Avoid_: CollectAudioLogs dump, pump/glitch trace
