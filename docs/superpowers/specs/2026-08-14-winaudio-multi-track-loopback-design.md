# WinAudio Multi-track Loopback Design

**Status:** canonical for product behavior (Track and multi-path capture).  
**Tracker copy:** `.scratch/loopback-tracks/spec.md` (`ready-for-agent`).  
**Glossary:** `CONTEXT.md`.  
**Map:** `.scratch/loopback-tracks/map.md`.

This document supersedes product behavior in `2026-07-09-winaudio-system-loopback-design.md` (one session per Loopback page, page-level silent-render checkbox) and treats `docs/superpowers/plans/2026-07-13-winaudio-application-loopback.md` as a historical executed plan. WASAPI mechanisms in those files (Shared-only, `AUDCLNT_STREAMFLAGS_LOOPBACK`, process-loopback activation, silent-render helper) remain useful background; they are not the source of truth for how many captures a page may run or how the user starts and stops them.

## Problem Statement

I use WinAudio to compare system output and individual process trees. Today the Loopback page and the Application Loopback page each run only one capture at a time. To watch a second render device or a second PID I have to stop the capture I am already looking at. I need several independent captures alive together, each with its own picture and, when I choose, its own WAV. I do not want them mixed into one stream or one file.

Monitor delayed listen should stay the single pair I already understand. CLI should start several captures in one command so I can script the same comparison.

## Solution

WinAudio gains a global **Track**: a unidirectional live stream the user creates and destroys on its own. A Track is either a **Capture Track** (one **Capture source**) or a **Render Track** (one render endpoint). Create starts immediately; there is no idle Track. Capture source is the recipe; Track is the instance.

- The System Loopback page is a list of System Loopback Capture Tracks. The Application Loopback page is a list of Application Loopback Capture Tracks. Each page: create one, destroy one, destroy all **on this page only**.
- Each live Capture Track has its own scope tap and, on a loopback page, its own stacked Capture-only **Chart Host** (full wave + spectrogram, multi-channel split inside that Track). The page scrolls. Freeze and the linked time axis are per Track. Tracks do not share an axis or a format.
- GUI WAV path is optional per Track (omit = charts only). CLI `capture` requires a WAV path per Track. One file per Track; no mixdown.
- **Monitor** remains one session: exactly one Capture Track plus at most one Render Track. DelayFifo belongs to the Monitor. GUI Monitor and CLI `monitor` do not grow a multi-Track list.
- CLI `capture` is one process-lifetime **group** of Capture Tracks. Repeat `--track` for each member. No `--track` plus one `--out` remains one Capture Track. A group may mix Endpoint, System Loopback, and Application Loopback. `list` / `caps` do not change model.
- No product-side numeric cap. Live Tracks may bind equal recipes. Failures stay on the Track that hit them; siblings and the other Monitor side are not auto-stopped. GUI lists are empty every launch.

## User Stories

1. As a test engineer, I want the Loopback page to keep a list of Capture Tracks instead of a single Start/Stop session, so that I can watch more than one render device at once.
2. As a test engineer, I want the Application Loopback page to keep a list of Capture Tracks, so that I can watch more than one process tree at once.
3. As a test engineer, I want each GUI loopback page to accept only its own Capture source kind, so that I do not mix device pickers and PID pickers on one page.
4. As a test engineer, I want to create a Capture Track by choosing a Capture source (and optional WAV and format / silent render) and confirming, so that capture starts immediately without a second Start step.
5. As a test engineer, I want creating a Track that fails open or start not to leave an idle row, so that the list only contains live instances or clearly failed ones.
6. As a test engineer, I want to optionally attach a WAV path when I create a GUI Capture Track, so that I can record that path without forcing a file on every visual session.
7. As a test engineer, I want a GUI Capture Track with no WAV path to still run and show charts, so that I can inspect loopback without choosing a file first.
8. As a test engineer, I want each Capture Track that has a WAV path to write only its own file, so that two live Tracks never share or mix a recording.
9. As a test engineer, I want a WAV write failure to mark only that Track, so that siblings on the same page keep running.
10. As a test engineer, I want to destroy one Capture Track on a page, so that I can drop a path I no longer care about without stopping the others.
11. As a test engineer, I want destroy to stop that Track’s capture, stop its silent render helper if it has one, close its WAV, drop its scope tap, and remove it from the page list, so that nothing of that instance keeps running or occupying UI.
12. As a test engineer, I want a destroy-all control on the Loopback page that destroys only System Loopback Capture Tracks on that page, so that I can clear the page without touching Application Loopback or Monitor.
13. As a test engineer, I want a destroy-all control on the Application Loopback page that destroys only Application Loopback Capture Tracks on that page, so that the other page and Monitor keep running.
14. As a test engineer using Monitor, I want the page to remain a single Monitor pair (one Capture Track and at most one Render Track), so that delayed listen does not become a multi-output mixer.
15. As a test engineer using Monitor, I want to stop Monitor with the existing session stop, so that I do not have a second “destroy all” that reaches across pages.
16. As a test engineer, I want DelayFifo to stay part of Monitor delay, not a Track, so that loopback pages never grow a playback path by accident.
17. As a test engineer, I want silent render to remain a System Loopback keepalive helper, not a Render Track, so that destroy-all does not look like I had extra playback Tracks.
18. As a test engineer, I want each live Capture Track to have its own scope tap, so that I can see that path’s waveform and spectrogram without mixing samples from another Track.
19. As a test engineer, I want multi-channel Capture Tracks to keep the existing per-channel chart split (up to the current channel cap) **inside that Track**, so that channel views are not confused with extra Tracks.
20. As a test engineer, I want every live Capture Track on a loopback page to show a full stacked Chart Host, so that I can compare paths without selecting one at a time.
21. As a test engineer, I want freeze on one Track’s host to leave other Tracks’ charts updating, so that I can pause one picture while watching another.
22. As a test engineer, I want each Track’s wave and spectrogram (and its channels) to share one time axis that is not shared with other Tracks, so that 48 kHz and 44.1 kHz histories do not pretend to align.
23. As a test engineer, I want to request a format per Track at create, so that I can compare negotiated formats without a page-wide format box.
24. As a test engineer, I want actual format to appear as that Track’s status, so that sr/ch on its Chart Host match the stream.
25. As a test engineer, I want silent render chosen per System Loopback Track at create (default on, immutable while live), so that I can run one path with keepalive and one without.
26. As a test engineer, I want to create another Capture Track for a Capture source I just destroyed, so that teardown of an instance does not retire the device or PID.
27. As a test engineer, I want to create a Capture Track for a source while another Track bound to the same source is still destroying, so that the UI does not lock the recipe during teardown.
28. As a test engineer, I want two live Tracks to be allowed to bind equal recipes, so that I can compare silent render or format on the same endpoint or PID.
29. As a test engineer, I want occupancy or platform errors from a second equal source to show up as that Track’s error, so that the product does not invent a “source busy” lock.
30. As a test engineer switching tabs, I want Tracks on a hidden loopback page to keep running, so that leaving the page is not destroy.
31. As a test engineer, I want Monitor’s pair and both loopback page lists to be able to run in the same process, so that I can listen on Monitor while recording loopback Tracks (without mixing them).
32. As a test engineer, I want a capture open/start failure or a vanished source to Error only that Capture Track, so that siblings keep recording and drawing.
33. As a test engineer, I want a silent render helper failure to mark only that helper, so that capture on that Track continues.
34. As a test engineer on Monitor, I want a failed Capture Track or Render Track to report Error without auto-stopping the other side, so that I can observe a half-dead pair as a test phenomenon.
35. As a test engineer, I want Monitor’s DelayFifo not to be torn down automatically on one-side Error, so that I stop the session myself when I am done looking.
36. As a CLI user, I want one `capture` invocation to start a group of Capture Tracks, so that I can script a multi-path recording without a GUI.
37. As a CLI user, I want every Capture Track in that group to require its own `--out`, so that the command cannot imply a shared or mixed WAV.
38. As a CLI user, I want to write `capture --track --out a.wav --loopback --track --out b.wav --pid 4242`, so that one process records mixed kinds.
39. As a CLI user, I want a command with no `--track` and one `--out` to remain one Capture Track, so that existing scripts keep working.
40. As a CLI user, I want `--pid` (optional `--exclude-tree`), else `--loopback`, else Endpoint, mutually exclusive in a segment, so that I do not need `loopback:` prefixes.
41. As a CLI user, I want `--format` and `--no-silent-render` inside the `--track` segment, so that those choices stay per Track.
42. As a CLI user, I want `--backend` and `--seconds` once per group, so that process lifetime and backend kind stay shared.
43. As a CLI user, I want `--pid` plus `--loopback` in the same segment to be a usage error, so that a bad recipe fails before open.
44. As a CLI user, I want Ctrl+C or `--seconds` to stop the whole group together, so that a script has one lifetime.
45. As a CLI user, I want some Tracks in the group to be allowed to Error while others keep writing, so that one bad path does not abort the recording set.
46. As a CLI user, I want a non-zero exit code when any Track failed, so that automation can detect partial failure.
47. As a CLI user, I want `list` and `caps` to keep their current model, so that discovery commands do not grow Track lists.
48. As a CLI user, I want `monitor` to stay a single Monitor pair, so that delayed listen on the command line matches the GUI Monitor limit.
49. As a CLI user, I want the capture group itself not to be a Track, so that logs and status talk about member Tracks, not a fictional mixed stream.
50. As a test engineer, I want Application Loopback create to still accept a PID that is not in the session list, so that I can target a process that has not shown a session yet.
51. As a test engineer, I want IncludeTree vs ExcludeTree to remain a property of the Application Loopback Capture source bound to that Track, so that two Tracks can express different modes on the same PID.
52. As a test engineer, I want System Loopback Capture sources to remain render endpoints, not fake capture devices, so that existing device language stays intact.
53. As a test engineer, I want Exclusive mode to remain rejected for loopback Tracks, so that Shared-only loopback is unchanged.
54. As a test engineer, I want no resampler and no mixdown when two Tracks have different formats or rates, so that the tool does not pretend they are one stream.
55. As a test engineer, I want “independent view” to mean samples from one Track’s scope tap only, so that overlaying or summing Tracks is not offered.
56. As a test engineer, I want destroy of a Track not to close the render device or kill the target process, so that the Capture source recipe remains usable.
57. As a test engineer, I want a new Track after recreate to have a new lifetime, new scope tap, and its own optional WAV, so that I do not inherit the destroyed instance’s buffers or file handle.
58. As a test engineer, I want the empty loopback page (no Tracks) to be a valid state, so that I am not forced to keep a placeholder Track.
59. As a test engineer, I want every GUI launch to start with empty Track lists, so that restart does not auto-grab devices or processes.
60. As a test engineer, I want no product-side maximum number of live Tracks, so that I can open as many paths as the machine will take.
61. As a documentation reader, I want Track, Capture Track, Render Track, Capture source, and Monitor to match `CONTEXT.md`, so that GUI copy, CLI help, and this spec do not invent synonyms.
62. As an implementer writing tests, I want a Capture Track list I can create/destroy/destroy-all/poll without WASAPI, so that CI can lock lifetime rules.
63. As an implementer, I want each live Track’s audio path to stay injectable via the existing backend factory style, so that I do not need real devices to prove a Track pumps.
64. As an implementer, I want each Track’s scope tap to be readable through the existing Scope Reader / Chart Data Pipeline ideas, so that chart tests do not take a dependency on ImGui.
65. As an implementer, I want GUI drawing and real WASAPI to stay outside the lifetime contract tests, so that create/destroy rules do not rot behind click tests.
66. As an implementer, I want this spec not to name `MonitorEngine` vs a new type as the required shape, so that the implementation plan can choose composition behind the Capture Track list.
67. As a test engineer on Monitor, I want creating extra Capture Tracks on that page to be impossible, so that the single-pair limit is a product rule, not a missing button.
68. As a test engineer, I want CLI `play` / `probe` to stay outside this spec’s Track group, so that playback-from-file is not silently redefined as multi Render Tracks.

## Implementation Decisions

- Domain language is the `CONTEXT.md` glossary. Track is the live instance; Capture source is the recipe; Monitor is the only named session; silent render is not a Track; a CLI capture group is not a Track.
- One new testable seam: a **Capture Track list**. Operations: create (bind Capture source + optional WAV + per-Track format / silent render, start immediately), destroy one, destroy all, poll per-Track status. GUI loopback pages each own one list. CLI `capture` uses the same list idea as one group with a single process lifetime.
- Reuse the existing backend-factory injection for each live Track’s pump (fake backends in tests). Reuse Scope Reader / Chart Data Pipeline for per-Track taps. Reuse CLI option-parser tests for `--track` segmentation. Do not use ImGui or real WASAPI as the lifetime contract.
- The spec does **not** mandate one existing engine object per Track or a new multi-Track engine type. The implementation plan chooses composition behind the list.
- No second Start/Stop vocabulary on loopback pages. Create starts; destroy stops and removes.
- Destroy all is scoped to that list / page, never to the whole application and never to Monitor.
- WAV is per Capture Track. GUI path optional; CLI path required. One file per Track; no shared file; no mixdown into one file.
- Failures stay on the Track that hit them: WAV write, capture open/start, runtime source loss, silent render helper. Siblings on the page or in the CLI group keep running. CLI `--seconds` / Ctrl+C still stop the whole group; exit code may be non-zero if any Track failed.
- Monitor: a failed Capture Track or Render Track is Error (overall too). The other side is not auto-stopped; DelayFifo is not auto-torn down. The user stops the session.
- No product-side maximum number of live Tracks (page, process, or kind). No soft warning threshold. Do not document a number as a Windows / WASAPI limit. Primary sources do not state a concurrent-loopback cap.
- GUI Track lists do not persist across process restart. Every launch is an empty list. Window-layout persistence may remain chrome-only.
- Same Capture source may be used to create a new Track while another instance is destroying, and two **live** Tracks may bind equal recipes (same kind + device, or same PID + mode). Same process name with a different PID is not equal. IncludeTree vs ExcludeTree on the same PID is not equal. No product-level “source in use” lock.
- Loopback pages stack one Capture-only Chart Host per live Capture Track (prototype variant B). Chart freeze, linked time axis, requested/actual format, and (for System Loopback) the silent render toggle are per Track. Silent render is chosen at create, default on, immutable while live.
- Monitor remains one Capture Track + at most one Render Track. DelayFifo belongs to Monitor.
- Loopback stays Shared-only. No application resampler. No mixdown of Tracks.
- CLI `capture` spelling: repeatable `--track` segments; per-segment `--out`, `--format`, `--no-silent-render`, `--pid` / `--exclude-tree` / `--loopback` / `--device`. No `--track` + one `--out` = one Capture Track. `--pid` and `--loopback` in the same segment is a usage error. `--backend` once per group. No manifest file.
- Do not change `list` / `caps` models.

## Testing Decisions

- Good tests assert external lifetime and isolation behavior, not which concrete engine class sits under a Track, and not ImGui widget IDs.
- Primary tests target the Capture Track list: create starts a live member; destroy removes it and releases its tap/WAV/helper; destroy all clears only that list; a second list (other page or Monitor) is unaffected; optional vs required WAV; WAV write failure does not destroy siblings; create during destroy of an equal source is allowed at the list API; two live equal recipes are allowed; a failed create does not stop siblings.
- Those tests must run without audio hardware, using the existing fake-backend factory pattern already used for Monitor session tests.
- Scope tap independence: reading one Track’s tap must not require another Track’s samples. Prefer the existing Scope Reader / Chart Data Pipeline test style.
- Loopback source representation and Shared-only rejection stay covered by the existing loopback / monitor session tests; extend them only where a Track list must plumb a Capture source through.
- CLI parser tests belong next to the current CLI options tests: `--track` segmentation, one-`--out` compat, `--pid` vs `--loopback` vs endpoint, `--pid`+`--loopback` rejected, each segment has `--out`, per-segment `--format` / `--no-silent-render`. No WASAPI.
- Do not use GUI click-through or `PrintWindow` as the contract for create/destroy.
- Real-device overlap (two loopbacks on one endpoint, two process loopbacks on one PID) is manual smoke, not CI.

## Out of Scope

- Mixing multiple Tracks into one audio stream or one WAV.
- Multi-Track GUI Monitor or multi-Track CLI `monitor`.
- Mixing Capture source kinds on one GUI page.
- waveIn / waveOut, an in-app resampler, Exclusive-mode loopback.
- Redefining CLI `play` / `probe` as multi Render Tracks.
- Inventing a documented Windows concurrent-loopback guarantee.
- Persisting GUI Track lists or adding a preset / start-all model.
- Mandating a specific engine class layout in this spec.

## Further Notes

- Wayfinder map (all tickets resolved): `.scratch/loopback-tracks/map.md`.
- Chart layout prototype (variant B won): `.scratch/loopback-tracks/prototype/multi-track-chart-presentation.html`.
- WASAPI research: `.scratch/loopback-tracks/research/wasapi-concurrent-loopback-limits.md`.
- Next flow: `/to-tickets` then `/implement` per ticket in a fresh session. Do not implement from this spec in the same wayfinder thread.
