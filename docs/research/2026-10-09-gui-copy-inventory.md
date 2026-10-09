# GUI copy inventory (2026-10-09)

Question: besides `AppUiText.h`, which user-visible sentences in the WinAudio window are hardcoded?

This note is an inventory the GUI-copy spec can treat as copy. It does not propose a catalog shape.

## Method

Primary sources only:

- `src/gui/` (every translation unit that can draw or title the window)
- GUI-facing tests under `src/tests/` that pin `wa::ui_text` or the strings those widgets consume
- Core helpers the window prints (Pipeline graph, attach banner, layout labels, session state)
- Vendored ImGui localization / table context menu

Not used as sources: README, specs, screenshots, CLI.

`CONTEXT.md` (Language / GUI copy) already defines GUI copy as the sentences a person reads in the window, including log-panel chrome and app-authored status lines. Channel-ident speech, CLI, and core/spdlog runtime logs are out of that set.

## Answer in brief

`wa::ui_text` in `src/gui/AppUiText.h` holds the later-page strings (Application Loopback, Render, Pipeline, charts freeze/zoom, dump buttons, log `+/-` / `(empty)`, OS-sound labels). The rest of the window is still English literals in `src/gui/AppUi.cpp`, plus a few helpers in `src/core/` that the window prints.

The largest holes vs `AppUiText.h` are:

- tab names `Monitor` and `Loopback`
- Monitor / Loopback / Render chrome: `Refresh devices`, `Devices`, `Source`, `Control`, `Status`, `Start` / `Stop`, `Backend`, `Delay (ms)`, `同步播放 (playback)`, `System audio`, `Capture device` / `Render device`, `Capture capabilities...`
- Format lines and combo origin/support words
- modal titles `Audio parameters (advanced)` / `Device capabilities` and every `Close` button
- Pipeline Live-session table headers
- chart titles, `Hz` / `dB` / `-inf` / `1k` ticks
- log-panel `Clear`, level names, and every AppUi-authored status line

ImGui’s built-in close-X is not shown (modals pass `p_open == nullptr`). The visible word `Close` is an `AppUi.cpp` button. ImGui table header right-click still shows ImGui loc strings on the resizable Pipeline sessions table.

## 1. Already in `wa::ui_text`

Source: `src/gui/AppUiText.h`. Pinned by `src/tests/test_audio_session_enumerator.cpp`, `src/tests/test_live_session_list.cpp`, `src/tests/test_render_track_list.cpp`, and (labels/failure lines) `src/tests/test_os_sound_ui.cpp`.

### Tabs, empty hints, OS sound

| Constant | Sentence |
|---|---|
| `kApplicationLoopbackTab` | Application Loopback |
| `kRenderTab` | Render |
| `kPipelineTab` | Pipeline |
| `kLoopbackEmptyHint` | Create a Track to capture system audio. |
| `kApplicationLoopbackEmptyHint` | Create a Track to capture application audio. |
| `kRenderEmptyHint` | Create a Track to play to a render endpoint. |
| `kMmsysCpl` / `kMsSettings` | mmsys.cpl / ms-settings |
| `kMmsysCplFailed` / `kMsSettingsFailed` | mmsys.cpl: ShellExecute failed / ms-settings: ShellExecute failed |

`kMmsysCplFailed` / `kMsSettingsFailed` are also pushed into the log panel (`AppUi::openOsSound`). Parent map: those app-authored status lines are GUI copy.

### Application Loopback / Loopback shared controls

Refresh, PID, Exclude, Sessions, Format reference, None, Default render device, Automatic, Capabilities..., Create Track, Destroy, Destroy all, Tracks, Silent render keepalive, Options, Capture options, System default, Apply, Dump, Stop dump, Dump capture, Dump render.

### Advanced options (Monitor modal + Capture options)

Help paragraph `kAdvancedOptionsHelp`; section titles Capture / Render; Set client properties; Hardware offload; Ducking opt-out; stream options None, Raw (bypass APO), Match format, Ambisonics, Post-volume loopback.

### Charts, log chrome, Render page, Pipeline page

Pause/Resume charts, PAUSED, Zoom out/in, Reset view; `(empty)`, `+`, `-`; Render endpoint/Layout/volume/pause/continue/play-once/No phrase/All channels paused/custom-ignored; channelPhraseText (Front left … channel 8); Pipeline Live sessions / Refresh / Show this process / empty and select hints / Processing graph / Probe this device / ETW unavailable|listening / Attach / Attached / Call log empty+waiting / Record pump metadata / xruns / table cols Iface Method Args HR Stream / mute yes|no / flow capture|render; `formatLiveSessionTooltip` template keys `process:`, `pid:`, `device:`, `volume:`, `mute:`, `state:`; attach-fail copies Cross-bitness / missing debug rights (duplicated with core `attachBlockText`; the window draws the Result message, not these constants).

`channelPhraseText` is the on-screen channel row label. Spoken WAV content is not GUI copy (`CONTEXT.md` _Avoid_: channel-ident speech). `PhraseCatalog` `reportName` is a different string (e.g. `LFE` vs `Low frequency`) and only appears in the AppUi status line `channel ident missing: …`.

## 2. Literals scattered in GUI files

Unless noted, all of these are `src/gui/AppUi.cpp`. None of them live in `wa::ui_text`. GUI tests do not `EXPECT_STREQ` the Monitor/Loopback tab names, `Refresh devices`, `Close`, or `同步播放 (playback)`.

### Window chrome (`src/gui/main.cpp`)

- OS title bar / taskbar: `L"WinAudio"` (`CreateWindowW`, line 41). Visible.
- Inner ImGui root `Begin("WinAudio", … NoTitleBar)` (AppUi.cpp ~739): ID only, not drawn.
- WNDCLASS name `L"ImGui Example"`: not shown as window copy.

### Tabs

- `Monitor` (`BeginTabItem`, ~742)
- `Loopback` (~746)
- Application Loopback / Render / Pipeline: from `ui_text` (~750–758)

### Monitor left panel (`drawLeftPanel`)

| Kind | Sentence |
|---|---|
| Separator | Devices, Control, Status |
| Button | Refresh devices |
| Button | Capture capabilities... (not `kCapabilities` “Capabilities...”) |
| Combo captions | Capture device, Render device |
| Combo empty | (no devices) |
| Combo | Backend; items WASAPI-Shared, WASAPI-Exclusive |
| Slider | Delay (ms) |
| Buttons | Start, Stop |
| Checkbox | 同步播放 (playback) |
| Progress overlay | cap, ren |
| Popup open | Audio parameters (advanced) |
| Caps context | Monitor capture device |

Default-device marker `* ` in combo previews is punctuation on a device name (device name is not copy).

### Loopback left panel

| Kind | Sentence |
|---|---|
| Separator | Source, Control |
| Button | Refresh devices |
| Combo | System audio |
| Combo empty | (no render devices) |
| Caps context | System Loopback render device |
| Popup | Device capabilities |
| Progress overlay | level |

Tracks / Create / Destroy / Dump / Silent render keepalive / Capabilities... come from `ui_text`.

### Application Loopback left panel

Most controls are `ui_text`. Scattered:

- Sessions empty: `(none)`
- Format-reference refresh: `Refresh devices` (`Button("Refresh devices##appLoopbackReference")`; visible label is Refresh devices)
- Separator Control
- Caps context: Application Loopback format reference
- Progress overlay: level

### Render left / track list

`ui_text` covers endpoint/layout/create/destroy/channel controls. Scattered:

- Button Refresh devices
- Combo empty (no render devices)
- Separator Control
- Client-format line template `%u Hz, %u-bit` (numbers are not copy; “Hz,” / “-bit” are)
- Read-only rate/bits line uses the same “Hz, …-bit” pattern for the 48 kHz / 16-bit client format

### Format region and format recipe (Monitor + loopback pages)

Drawn by `drawFormatRegion` and `drawFormatRecipe`:

- Line prefix `Format:`
- Suffix ` (default)` when Monitor has not applied a requested format
- Suffix `f` for float (format syntax, not a sentence)
- Suffix ` (N valid)` from local `formatLabel` (N is a number; “valid” is copy)
- Combo item `System default` / `System default##0` — **same words as `kSystemDefault`, but a separate literal** in `drawFormatRegion` (Monitor). Loopback/App Loopback pass `kSystemDefault` / `kAutomatic` into `drawFormatRecipe`.
- Candidate extra text ` [Mix, Device, OEM, Standard; Exact|Closest]` via `formatOriginsLabel` / `supportLabel` (`Mix`, `Device`, `OEM`, `Standard`, `Exact`, `Closest`, placeholder `-`)

Custom field placeholder `48000/16/2` is a typed example, not a sentence.

### Advanced / Capture options / Caps modals

| Kind | Sentence | Where |
|---|---|---|
| Modal title | Audio parameters (advanced) | Monitor Options |
| Modal title | Capture options | `kCaptureOptionsPopup` |
| Modal title | Device capabilities | Monitor / Loopback / App Loopback |
| Button | Close | all three modals (lines ~584, ~2031, ~2491) |
| Running banner | Running: parameters are read-only. Stop the monitor to change them. | advanced modal |
| Combo | Category | capture + render columns |
| Combo items | Other, Communications, Media, Movie, Game chat, Speech, Sound effects, Game media | `kAudioCategories` in AppUi.cpp (not `ui_text`) |
| Combo | Stream option | label hardcoded; items from `kAdvancedStreamOptions` |
| Input | Buffer (ms) | |
| Button | Reset to system defaults | capture column + render column |
| Caps table headers | Mix, Device, OEM, Format, Layout, Sources, Shared, Exclusive | |
| Caps empty cell | `-` | |

### Pipeline page (beyond `ui_text`)

Live-session table headers (not in `ui_text`): **Flow, Process, PID, Device, Vol, Mute, State**.

On-page values:

- Flow / mute words from `ui_text` (`capture`/`render`, `yes`/`no`)
- Process / device names: not copy
- PID / volume numbers: not copy
- State: core `sessionStateName` (Inactive / Active / Expired / Unknown) — copy, see §3
- Attach banner: `pipelineAttachBanner_` = `Result.message` from attach (copy templates in core, HRESULT not copy)
- Graph node title/param sentences: core `assemblePipeline`, see §3
- Call-log cells iface/method/args: COM identifiers, not copy; HR / Stream: numbers, not copy

### Charts (`chartTitle`, `drawComboPanel`, plots)

| Sentence | Notes |
|---|---|
| Capture waveform + spectrogram | drag preview + heading |
| Render waveform + spectrogram (delayed) | |
| Capture channels: %u / %u channels shown | numbers not copy |
| Capture Ch N waveform | N not copy |
| Capture Ch N spectrogram | N not copy |
| Hz | Y unit |
| dB | Y unit |
| Tick labels 20, 30, 40, 60, 100, 200, 300, 400, 600, 1k, 2k, 3k, 4k, 6k, 10k, 20k | |
| Tick labels 0, -6, -12, -24, -48, -inf | `-inf` is copy |

ImPlot plot IDs (`##capWave`, colormap name `WaSpectrogram`) are not visible.

### Track captions and status (assembled; templates are copy)

Loopback/App stacked chart caption (`drawStackedCaptureTrackHosts`):

- `Track ` + id
- `  pid=` + number (Application Loopback)
- `  ` + `IncludeTree` / `ExcludeTree` (`processLoopbackModeName` in `src/core/IAudioBackend.h`)
- `  ` + rate + ` Hz / ` + channels + ` ch`
- `  [` + `t.message` + `]` on Error (`t.message` is a core Result string; HRESULT/API-where is not copy)

Left-list rows:

- `id=%llu  %s  sr=%u  xrun=%llu` (Loopback)
- `id=%llu  %s  pid=%u  %s  sr=%u  xrun=%llu` (Application Loopback)
- `id=%llu  %s` (Render)
- Stream-state words **Idle, Running, Error** (local `ss[]` / `states[]` in AppUi.cpp)

Monitor Status block:

- `overall=%s  cap=%s  ren=%s  sr=%u  delay=%ums`
- `fifo=%.0fms  drift=%llu  xrun c/r=%llu/%llu`
- `frames c/r=%llu/%llu`

### Log-panel chrome (not `ui_text` except empty/+/-)

`drawLogPanel`:

- Level combo has **no** visible “Level” caption (`##loglevel`). Visible items: **Trace, Debug, Info, Warn, Err**
- Button **Clear** (`Clear##logRegion`)
- Collapsed overflow ellipsis `...` (truncation chrome)
- Expanded body is the line list (mix of AppUi status lines and spdlog lines)

### Other GUI files

- `src/gui/OsSoundUi.h`: labels/failure lines from `ui_text` only
- `src/gui/CreateRecipe.h`: error `invalid format` (also AppUi Monitor Apply, Render custom Apply). Pinned by `src/tests/test_create_recipe.cpp` and `src/tests/test_render_track_list.cpp`
- `src/gui/DumpUi.cpp`: no product sentences in the window. Folder picker is `IFileOpenDialog` (Windows Shell copy). Explorer `ShellExecute failed` is WA_LOG only, filtered out of the log panel only if it matched `LogRegionPrefs::` (it does not; it is still a core/spdlog line, not an AppUi status line)
- `src/gui/ApplicationLoopbackUiModel.h`, `ChartHost.h`, `CaptureChannelView.h`, `ChartDataPipeline.*`, `ChartsFreezePolicy.h`, `ChartsTimeZoomPolicy.h`, `LogRegionPrefs.h`, `Spectrogram.*`: no window sentences

## 3. Runtime-assembled sentences

Treat the **template words** as GUI copy. Do not treat device names, process names, PIDs, rates, bit depths, channel counts, masks as hex, HRESULT, dump paths/filenames, or COM iface/method/args as copy.

| Template | Owner | Runtime slots (not copy) |
|---|---|---|
| `Format: {fmt}[(default)]` | AppUi | sampleRate/bits/channels |
| `{fmt} (N valid)` | AppUi `formatLabel` | N |
| `{fmt}  [{origins}; {support}]` | AppUi | format numbers |
| `{layout} / mask 0x{X}` or `{N} channels` | `channelLayoutLabel` in `src/core/AudioFormatStr.h` | N, mask |
| `%u Hz, %u-bit` | AppUi Render | rate, bits |
| `Track {id}  pid={pid}  {IncludeTree\|ExcludeTree}  {sr} Hz / {ch} ch` | AppUi + `processLoopbackModeName` | id, pid, sr, ch |
| `id=… Idle\|Running\|Error …` | AppUi | numbers |
| `Capture Ch {n} waveform/spectrogram` | AppUi | n |
| `Capture channels: {shown} / {actual} channels shown` | AppUi | counts |
| Live-session tooltip (`formatLiveSessionTooltip`) | `ui_text` | process, pid, device, volume, state |
| Graph `{title} [{Observed\|…}]` and `{key}: {value} [{kind}]` | `PipelineGraph.cpp` + `observationKindName` | device/process/format/HRESULT values |
| Probe `{label}: {effect} ON\|OFF [(settable)]` | `PipelineGraph.cpp` | vendor effect name if unmapped GUID |
| `{prefix}{id}` destroy/create/dump status lines | AppUi | id, path |
| `{prefix}: {Result.message}` refresh/error lines | AppUi prefix + core message | HRESULT / API-where in message |

Layout friendly names pinned by `src/tests/test_render_track_list.cpp`: Mono, Stereo, Quad, 5.1, 7.1, plus `mask 0x…` / `N channels`.

`IncludeTree` / `ExcludeTree` are enum spellings shown in the window; they are still user-visible words.

Dump filename next to Dump/Stop dump is a filesystem name, not copy.

## 4. Core-owned strings the window prints

These are not in `AppUiText.h`. The window shows them on Pipeline / Render / attach banner / format combos. Spec can treat the English words as GUI copy; numbers/HRESULT/device names still not.

### Pipeline graph (`src/core/PipelineGraph.cpp`, tests in `src/tests/test_pipeline_graph.cpp`)

Node titles: Hardware; Driver / KS; EFX (endpoint); MFX (mode); SFX (stream); Engine SRC / mix format; Session; App; Audio engine (Shared).

Kind tags (`observationKindName`): Observed, Probed, Inferred, Skipped, Unknown.

Fixed param keys/values (non-exhaustive of every branch, complete for hardcoded English):

- APO role keys EFX / MFX / SFX; value `(none registered)`
- `(no advertised effects)`; suffixes ` ON`, ` OFF`, ` (settable)`
- device format, OEM format
- topology / no readable controls
- note / kernel path not dumped in v1
- exclusive / software EFX usually not on this path; exclusive / bypassed
- SysFx / disabled
- category / unknown (or ETW mapped name)
- RAW / SFX skipped; MFX may still load; RAW / SFX not used
- instance / per-stream; not the watched session
- mix format / unavailable; note / mix format is the engine, not the app stream format
- MATCH_FORMAT / requested | not requested
- pid, volume, mute true|false, device, process, flow capture|render
- Initialize HRESULT (value is a number, not copy)
- app stream format (value is a format string)

Hardware keys from `src/core/EndpointGraphReader.cpp`: mute true|false, volume `{n} dB`, AGC on|off. Part display names from the driver are not copy (`IPart::GetName`).

Probe recipe labels from `src/core/SharedProbe.cpp`: Default, Communications, Raw. Effect type names from `guidToName` (Acoustic Echo Cancellation, Noise Suppression, … Deep Noise Suppression, fallback Unknown effect or a GUID). GUID fallback is not copy.

ETW category map in `src/core/EtwInitialize.cpp` `categoryNameFromValue` (Other, ForegroundOnlyMedia, Communications, … VoiceTyping) can appear as the graph `category` value. These identifiers are not the same spelling as Monitor `kAudioCategories` (`Sound effects` vs `SoundEffects`).

### Session state (`src/core/LiveSessionEnumerator.cpp`)

`sessionStateName`: Inactive, Active, Expired, Unknown. Shown in the Pipeline State column and in the tooltip `state:` line.

### Attach banner (`src/core/OnDemandAttach.cpp`, tests `src/tests/test_on_demand_attach.cpp`)

Drawn with `ImGui::TextWrapped` when not attached. User-facing sentences:

- Attached (success path uses `ui_text::kPipelineAttached` instead of this)
- Attach failed: invalid PID
- Attach failed: cannot attach to this process
- Attach failed: refusing audiodg
- Attach failed: cross-bitness (also `kPipelineCrossBitness`)
- Attach failed: missing debug rights (also `kPipelineNoDebug`)
- Attach failed (fallback)
- Attach: hook install failed in target
- Attach: hook install did not finish; restart the target app and retry
- Attach: LoadLibraryW not found
- Attach: inject timed out
- Attach: LoadLibraryW failed in target
- Attach: VirtualAllocEx / WriteProcessMemory / CreateRemoteThread / CreateFileMapping / MapViewOfFile via `HrToResult` → `{where} failed: hr=0x…` (`src/core/ComUtil.h`). Template words ` failed: hr=` are copy-ish; the hex is not copy.

`etwWatchStatusText` (`src/core/EtwInitialize.cpp`) duplicates ETW unavailable / listening; the Pipeline page draws `ui_text` for those two. `ETW stopped` is not drawn.

### SharedProbe / create errors shown as graph values or log prefixes

- `exclusive probe refused` (probe error value if Exclusive)
- `invalid format` (`CreateRecipe.h` / `renderLayoutFromCustom`)

## 5. ImGui / ImPlot / OS chrome

### ImGui built-in (ticket example: Close)

Modals call `BeginPopupModal(title, nullptr, AlwaysAutoResize)`. With `p_open == nullptr`, ImGui does **not** draw its title-bar close-X (`imgui.cpp` documents Close as available when `bool* p_open` is passed). The visible **Close** on Capture options / Audio parameters / Device capabilities is AppUi’s `Button("Close")`.

What ImGui can still show in this binary:

- Table header context menu on the **resizable** Pipeline sessions table (`ImGuiTableFlags_Resizable` in `drawPipelinePage`). Loc table in `third_party/imgui/imgui.cpp` `GLocalizationEntriesEnUS`:
  - Size column to fit
  - Size all columns to fit
  - Size all columns to default
  - Reset order (only if Reorderable; this table is not)
  - `<Unknown>` if a column name is empty
- Pipeline call-log table is not Resizable/Reorderable/Hideable, so that menu does not open (`TableOpenContextMenu` requires one of those flags).
- Other loc entries `(Main menu bar)`, `(Popup)`, `(Untitled)`, version string: not reached by this UI (single untitled-bar window, no menu bar).
- InputText has keyboard cut/copy/paste; this ImGui build does not draw a Cut/Copy/Paste menu in `InputTextEx`.
- Color-picker menus (RGB/HSV/Hex, Copy as..) are unused.

### ImPlot

Axes are unlabeled (`SetupAxes(nullptr, nullptr)`). No ImPlot built-in axis title is shown. Tick labels and Hz/dB are AppUi’s.

### Windows Shell / OS

- `IFileOpenDialog` folder pick (`DumpUi.cpp`): OS-localized dialog, not product copy
- Explorer after dump stop: OS window
- `mmsys.cpl` / Settings launched by ShellExecute: OS copy
- Win32 system menu / caption buttons on the `WinAudio` HWND: OS chrome
- `FormatMessageA` text inside `wa::log::hrName` on spdlog lines: OS, not GUI copy

## 6. Log panel: GUI-authored status lines vs core/spdlog

Two feeds share `logLines_`:

1. **AppUi `logLines_.push_back`** — parent map: GUI copy (fixed sentences + templates).
2. **`wa::log` callback** (`src/gui/main.cpp` `addCallbackSink` → `AppUi::pushLog`) — core/spdlog runtime logs, **not** GUI copy.

`pushLog` drops any line containing `LogRegionPrefs::` so ini load/save Warns stay out of the panel (`AppUi.cpp` ~347–350). They still go to `winaudio.log`.

### 6a. AppUi-authored status lines (GUI copy)

Prefixes/sentences in `AppUi.cpp`:

- application loopback refresh failed:
- pipeline refresh failed:
- pipeline endpoint snapshot failed:
- pipeline probe ignored: Live session list changed
- pipeline probe failed:
- pipeline probe finished with {n} recipe error(s)
- pipeline probe completed
- pipeline attach failed:
- pipeline attached pid=
- invalid format
- system loopback capabilities error:
- application loopback format reference error:
- pipeline ETW unavailable:
- pipeline live session watch unavailable:
- dump started {path}
- dump start error:
- dump stopped {path}
- dump stop error:
- loopback track destroyed id=
- application loopback track destroyed id=
- render capabilities error:
- render track destroyed id=
- render track created id=
- render error:
- render destroy all
- loopback track created id=
- loopback error:
- loopback destroy all
- application loopback create error: invalid PID
- application loopback track created id= {id} pid= {pid} mode= IncludeTree|ExcludeTree
- application loopback error:
- application loopback destroy all
- mmsys.cpl: ShellExecute failed / ms-settings: ShellExecute failed (`ui_text`)
- monitor started
- monitor error:
- monitor stopped
- channel ident missing: {reportName}

Trailing `r.message` / path / id / pid are not copy when they are HRESULT, API-where, or filesystem.

### 6b. Core/spdlog lines (not GUI copy; bound only)

Format (`src/core/Log.cpp`):

- pattern `%H:%M:%S.%e %v`
- control-path payload `{[T]|[D]|[I]|[W]|[E]} [{thread}] {module}::{call} args: {args} ret: {ret}`
- Trace: `[T] [{thread}] {module}::{call} frames=… flags=… hr=… {hrName}`

Default GUI level is Info (`main.cpp` `setLevel(Info)`; combo default index 2). Trace/Debug appear only after the user picks them. Typical Info/Warn/Err modules the panel can show: WasapiStream, MonitorEngine, CaptureTrackList, RenderTrackList, SharedProbe, Attach, EndpointGraph, LiveSession, DumpUi, OsSoundUi, plus others that `WA_LOG` at Info/Warn/Err.

These lines are instrumentation (`CLAUDE.md`: core never prints; `wa::log` is the sink). They are not GUI copy even though they occupy the same panel.

## Tests that pin copy

| File | What it pins |
|---|---|
| `src/tests/test_audio_session_enumerator.cpp` | Most `ui_text` Monitor-adjacent / loopback / charts / log +/- / OS sound |
| `src/tests/test_live_session_list.cpp` | Pipeline `ui_text`, tooltip assembly, attach/ETW constants vs core helpers |
| `src/tests/test_render_track_list.cpp` | Render `ui_text`, `channelPhraseText`, layout labels, `invalid format` |
| `src/tests/test_os_sound_ui.cpp` | mmsys/ms-settings labels and failure lines |
| `src/tests/test_create_recipe.cpp` | `invalid format` |
| `src/tests/test_pipeline_graph.cpp` | observation kind names and graph param sentences |
| `src/tests/test_on_demand_attach.cpp` | attachBlockText / install-fail sentences |
| `src/tests/test_etw_initialize.cpp` | etwWatchStatusText |

No GUI test pins `Monitor`, `Loopback`, `Refresh devices`, `Close`, `同步播放 (playback)`, Format: prefix, Pipeline table headers Flow/Process/…, or chart titles.

## Out of this inventory (not window copy)

- CLI usage / CLI status (`src/cli/`)
- Code comments, ImGui IDs (`##…`), colormap names
- Channel-ident WAV speech and filenames as audio
- `winaudio.ui.ini` keys
- README / maintainer docs
