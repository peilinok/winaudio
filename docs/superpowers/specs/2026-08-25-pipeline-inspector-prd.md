## Problem Statement

When another app starts microphone capture or playback, I cannot see what processing the audio went through from the hardware microphone to the app, or from the app writing samples to the hardware renderer — nor the parameters of each stage. Windows does not expose another process's in-process APO objects. Public session APIs also omit that process's stream Initialize arguments (format, share mode, RAW, category).

## Solution

A Pipeline Inspector tab reconstructs one capture or render path for a Live session I select. The graph shows stages with Observation kind (Observed, Probed, Inferred, Skipped, Unknown). I can On-demand attach into that process (same bitness) to intercept Core Audio COM and see a Call log of control-path arguments. Attach-before Initialize holes are filled from ETW, the endpoint graph, and a Shared probe. The Inspector never hooks audiodg APO COM and never claims Shared-probe effect types are the target's APO instances.

## User Stories

1. As an audio tester, I want a Pipeline tab besides Monitor / Loopback / Application Loopback, so that I can inspect other apps without stopping Tracks I already created.
2. As an audio tester, I want Live sessions on both capture and render endpoints listed, so that I see who is using a microphone and who is playing.
3. As an audio tester, I want each Live session row to show process name, PID, device, flow, volume, mute, and state, so that I can pick the right mixer row.
4. As an audio tester, I want the list to update when a process starts or stops a Live session, so that I do not have to guess whether capture has begun.
5. As an audio tester, I want to refresh the Live session list manually, so that I can recover if a notification was missed.
6. As an audio tester, I want this process hidden from the list by default, so that WinAudio's own streams do not clutter the radar.
7. As an audio tester, I want an option to show this process, so that I can verify Inspector behaviour against a Track I control.
8. As an audio tester, I want selecting a Live session to show an ordered processing graph, so that I can follow hardware mic → app or app → hardware.
9. As an audio tester, I want capture node order Hardware → Driver/KS → EFX → MFX → SFX → Engine SRC → Session → App, so that the graph matches the shared-mode engine.
10. As an audio tester, I want render node order to be the reverse of capture, so that playback is not drawn as if it were capture.
11. As an audio tester, I want every graph node and parameter labelled Observed, Probed, Inferred, Skipped, or Unknown, so that I do not treat guesses as facts.
12. As an audio tester, I want Observed and Probed visually distinct, so that I never read a Shared probe as the target's APO instances.
13. As an audio tester, I want mix format labelled as the engine mix, not as the app stream format, so that I do not misread Capabilities as the other process's WAVEFORMATEX.
14. As an audio tester, I want registered SFX, MFX, and EFX CLSIDs on the endpoint graph, so that I see what the device recipe installed.
15. As an audio tester, I want readable hardware volume, mute, and AGC when the topology exposes them, so that I see controls every shared stream actually passes.
16. As an audio tester, I want SysFx-disabled endpoints to show SFX and MFX as Skipped, so that I do not draw enhancements that are off.
17. As an audio tester, I want Exclusive (only with Observed evidence) to skip the shared engine nodes, so that I do not draw SFX/MFX/SRC on a path that bypassed them.
18. As an audio tester, I want RAW Observed to skip SFX, so that the graph matches the engine rule that RAW streams do not use SFX.
19. As an audio tester, I want to run a Shared probe on the same endpoint (Default, Communications, Raw) without Start and never Exclusive, so that I can see advertised effect types when I have not attached.
20. As an audio tester, I want a failed Shared probe (including when the target holds Exclusive) to leave the Live session radar intact, so that discovery does not die with the probe.
21. As an audio tester, I want probe results stored as Probed, so that I cannot confuse them with Hooked-stream Observed arguments.
22. As an audio tester, I want ETW Initialize hints matched by PID, a short time window, and device id, so that pre-attach category/RAW/HRESULT can be Observed when they actually belong to this Live session.
23. As an audio tester, I want unmatched ETW to stay Unknown rather than applying the latest Initialize on the machine, so that I do not glue the wrong stream onto this row.
24. As an audio tester, I want the Inspector to keep working when ETW cannot be enabled, with a clear unavailable status, so that I can still use radar, graph, probe, and attach.
25. As an audio tester, I want On-demand attach on the selected Live session PID, so that I intercept that process's Core Audio COM without injecting every audio process.
26. As an audio tester, I want attach to require the same bitness as the GUI, so that a 64-bit GUI does not pretend to hook a 32-bit target.
27. As an audio tester, I want the GUI to stay unelevated, so that Monitor and Loopback keep working without Administrator.
28. As an audio tester, I want attach without debug rights to fail closed with a banner, so that the Inspector does not look attached when it is not.
29. As an audio tester, I want attach not to launch the target suspended and not to restart the target's stream, so that I do not interrupt a call to obtain Initialize.
30. As an audio tester, I want Hooked-stream control-path arguments (Activate, Initialize, SetClientProperties, Start/Stop, GetService, session control, volume, clock, IAudioEffectsManager) in the Call log, so that I can read the real parameters.
31. As an audio tester, I want Initialize fields from a Hooked stream to override ETW patches on the graph and be Observed, so that attach-after facts win.
32. As an audio tester, I want pump GetBuffer/ReleaseBuffer omitted from the Call log by default, so that Initialize is not drowned in 10 ms noise.
33. As an audio tester, I want an option to record pump metadata into a small ring with xrun counts and never PCM, so that I can prove the stream is pumping without a disk flood.
34. As an audio tester, I want a Call log pane beside the graph, so that time-ordered COM detail and the path narrative stay separate.
35. As an audio tester, I want one Live session to show multiple Hooked streams if the process Initialized more than once, so that session and stream stay distinct.
36. As an audio tester, I want Core Audio intercept limited to the closed COM set (no XAudio2, DirectSound, or WinRT AudioGraph entry points), so that the product is not an API Monitor clone.
37. As an audio tester, I want no intercept of audiodg APO COM, so that I do not destabilize protected audio or fake APO internals.
38. As an audio tester, I want switching away from the Pipeline tab not to stop Monitor or Loopback Tracks, so that inspection is side-by-side with my own streams.
39. As an audio tester, I want CLI behaviour unchanged, so that scripts and `list`/`caps`/`capture`/`play`/`monitor` keep working.
40. As an audio tester, I want Unknown rather than invented APO knobs (EQ curves, NS strength), so that the tool stays honest.
41. As an audio tester, I want attach after Initialize to still show future calls immediately, so that Stop/Start and the next Initialize are Observed even if the first Initialize was missed.
42. As an audio tester, I want a 32-bit WinAudio GUI to attach to 32-bit targets the same way, so that same-bitness attach works on both official binaries.
43. As an audio tester, I want failed attach to keep using Shared probe and ETW on that Live session, so that I still have a reconstruction.
44. As a reviewer, I want Observation kind names stable in UI copy tests, so that labels cannot drift from the glossary.

## Implementation Decisions

- Respect ADR-0002: in-process Core Audio intercept on On-demand attach; no audiodg APO intercept; no system-wide API Monitor; GUI not always elevated.
- Add a Pipeline Inspector surface on the GUI. It does not own Tracks, Monitor, or WAV dumps.
- Keep Live session enumeration separate from Application Loopback's PID-deduped render list so that Inspector can show capture and render session instances.
- Join all reconstruction inputs through one snapshot → graph function. The landed join already takes Live session view, endpoint snapshot, ETW Initialize hint, and probe slices. Extend that snapshot with optional hooked-call records so attach overrides ETW without a second graph builder.
- Type shape from the landed join (prototype, not a demo): `ObservationKind { Observed, Probed, Inferred, Skipped, Unknown }`; `assemblePipeline(session, endpoint, etw, probes) → nodes`; hooked control-path fields, when present, are Observed and replace matching ETW fields.
- Call log is a separate view of the same hooked-call records: control-path retained; pump metadata off until enabled, then a small ring plus xrun counts, never PCM.
- Shared probe: three Shared-only recipes (Default, Communications, Raw); Initialize then query advertised effects; no Start; Exclusive probe fails closed.
- ETW Initialize: Microsoft-Windows-Audio PlaybackManager/Performance and Microsoft.Windows.Audio.Client AudioClientInitialize; match PID + short window + device id; no match → Unknown; enable failure → layer absent.
- On-demand attach: selected Live session PID only; same bitness; fail closed without debug rights; no auto-inject; no launch-suspended; no cross-bitness helper from the 64-bit GUI.
- Core Audio intercept closed set: device activate, IAudioClient*, capture/render clients, IAudioEffectsManager, session control, stream/channel volume, audio clock.
- Adapters (session watch, endpoint FX/topology, probe, ETW enable, inject) sit below the snapshot join. They must not become the test seam.
- CLI is unchanged.

## Testing Decisions

- Good tests assert external behaviour of the snapshot join and the Call log retention rules: node order, kind labels, RAW skips SFX, Exclusive skips engine, SysFx skips SFX/MFX, probe stays Probed, unmatched ETW does not invent HRESULT, hooked Initialize overrides ETW, pump default-off, pump ring drops oldest metadata, no PCM in log records. Tests do not inject into live processes, do not EnableTrace, and do not open real WASAPI devices.
- Primary seam (one): Pipeline Inspector snapshot in → graph nodes + Call log view out. Prefer extending the existing assemblePipeline join over adding parallel graph code. Feed canned Live session, endpoint, ETW, probe, and hooked-call records.
- Secondary only if the join cannot express it: Call log ring behaviour with canned intercept records (still no inject).
- Modules under test: pipeline join, Call log retention, Live session list shaping (sort/filter/hide-self) with fake rows — same style as existing session-list tests.
- Prior art: pipeline graph tests (capture/render order, RAW, Exclusive, Probed vs Observed); audio session enumerator tests (sort/dedupe without devices); stream params tests (Windows-free predicates). GUI copy constants follow existing UI text tests.
- CI must remain device-free. Real attach, real ETW, and real Shared probe are manual smoke, not gating.

## Out of Scope

- Hooking audiodg or IAudioProcessingObject; APO instance lists; EQ/NS internal knobs.
- Auto-inject; launch-suspended monitor; 32-bit helper spawned from the 64-bit GUI; always-admin GUI.
- XAudio2, DirectSound, and WinRT AudioGraph entry-point hooks.
- Exclusive Shared probe; forcing the target to re-Initialize; recording PCM from GetBuffer.
- CLI flags for Inspector; CollectAudioLogs-scale tracing; spatial object parameters; Time Travel Tracing.
- Changing Monitor, System Loopback, or Application Loopback stream behaviour.

## Further Notes

- Glossary: Live session, Hooked stream, Core Audio intercept, On-demand attach, Call log, Observation kind, Shared probe, AudioClientInitialize (ETW). Avoid calling a Live session a Track or a Monitor.
- Application Loopback session picker remains a different list (render, unique PID). Do not reuse it as the Inspector radar.
- Chromium and similar hosts may open WASAPI in a child PID; attach uses the Live session's process id, which is the WASAPI opener when the enumerator reports it.
- Design narrative also lives in the Pipeline Inspector design document and ADR-0002; this spec is the implementation contract.
