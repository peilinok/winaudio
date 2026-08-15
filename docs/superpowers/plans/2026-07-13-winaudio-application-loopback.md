# WinAudio Application Loopback Implementation Plan

> **Historical executed plan.** Do not treat this file as a living spec. Product behavior for Application Loopback as Capture Tracks (multi-path GUI/CLI, lifetime, isolation) is specified in [../specs/2026-08-14-winaudio-multi-track-loopback-design.md](../specs/2026-08-14-winaudio-multi-track-loopback-design.md). Process-loopback activation and session enumeration described here remain useful implementation history.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a fully working GUI Application Loopback tab that discovers active WASAPI audio-session processes, allows choosing or manually entering any PID, and captures that process tree through Windows process-loopback audio.

**Architecture:** Keep system loopback behavior intact and add application loopback as a third capture source. Put process-loopback activation and audio-session discovery in focused core files; keep `AppUi` responsible for presentation and start/stop wiring only.

**Tech Stack:** C++17, Win32 COM/WASAPI, `ActivateAudioInterfaceAsync`, `IAudioSessionManager2`, Dear ImGui, existing `MonitorEngine` visualization pipeline, gtest.

---

## Files

- Modify: `src/core/IAudioBackend.h`
  - Add `CaptureSourceKind::ApplicationLoopback`.
  - Add a PID field to `CaptureSource`.
- Create: `src/core/AudioSessionEnumerator.h`
- Create: `src/core/AudioSessionEnumerator.cpp`
  - Enumerate render endpoint audio sessions.
  - Resolve each session PID to a process name.
  - Sort by process name, then PID.
- Create: `src/core/ApplicationLoopbackCapture.h`
- Create: `src/core/ApplicationLoopbackCapture.cpp`
  - Implement the process-loopback capture backend and activation helper.
- Modify: `src/core/WasapiStream.h`
  - Move reusable capture loop/service hooks only if needed; otherwise keep endpoint/system loopback unchanged.
- Modify: `src/core/MonitorEngine.cpp`
  - Factory creates `ApplicationLoopbackCaptureStream` when source kind is application loopback.
- Modify: `src/core/CMakeLists.txt`
  - Add new core source files and required Windows libraries if needed.
- Modify: `src/gui/AppUi.h`
  - Add application loopback engine/status/session-list/PID-input/visual state.
- Modify: `src/gui/AppUi.cpp`
  - Add the new tab, session refresh, PID selection/input, start/stop, charts, and status.
- Modify: `src/tests/CMakeLists.txt`
  - Add new tests.
- Create: `src/tests/test_audio_session_enumerator.cpp`
  - Pure sorting/dedup helpers where possible.
- Modify: `src/tests/test_loopback.cpp`
  - Capture source representation and shared-only backend behavior.
- Modify: `src/tests/test_monitorengine.cpp`
  - Application loopback source reaches the backend factory and does not trigger system-loopback feedback/silent-render behavior.

## Requirements

- Application Loopback tab is visible next to `Monitor` and `Loopback`.
- On first draw/open, the tab refreshes the session process list automatically.
- Refresh button re-enumerates active audio-session processes.
- The process list is sorted by process name, then PID.
- Selecting a list row copies that PID into an editable PID input field.
- The PID input field accepts PIDs that are not in the session list.
- Start uses the PID input field, not the selected row as hidden state.
- Start rejects PID `0` and non-numeric text with a visible log/status message.
- Capture uses Windows process-loopback activation for the target PID with include-process-tree mode.
- The page reuses the existing waveform + spectrogram view.
- Existing Monitor/System Loopback behavior and tests stay green.

## Task 1: Failing Tests for Source Model and Monitor Wiring

- [ ] Add tests that fail before `ApplicationLoopback` exists:
  - `CaptureSource.CanRepresentApplicationLoopbackProcess`
  - `MonitorEngine.ApplicationLoopbackCaptureSourceReachesFactory`
  - `MonitorEngine.ApplicationLoopbackDoesNotStartSilentRender`
- [ ] Run:
  - `.\build\bin\Debug\WinAudioTests.exe --gtest_filter=CaptureSource.*:MonitorEngine.ApplicationLoopback*`
- [ ] Expected red state:
  - compile fails or tests fail because `ApplicationLoopback` / PID source fields do not exist.

## Task 2: Implement Capture Source Model and Factory Wiring

- [ ] Add `ApplicationLoopback` and `processId`.
- [ ] Update `MonitorEngine::makeBackend` to select an application-loopback backend.
- [ ] Add a temporary minimal backend class only as far as needed to compile the wiring test; real activation arrives in Task 4.
- [ ] Run the focused tests from Task 1 and make them pass.

## Task 3: Failing Tests for Session List Ordering and PID Selection Helpers

- [ ] Add testable helpers for sorting/deduping session process rows and parsing PID text.
- [ ] Tests:
  - `AudioSessionEnumerator.SortsByProcessNameThenPid`
  - `AudioSessionEnumerator.DeduplicatesByPid`
  - `AppLoopbackPid.ParseAcceptsManualPid`
  - `AppLoopbackPid.ParseRejectsZeroAndGarbage`
- [ ] Run:
  - `.\build\bin\Debug\WinAudioTests.exe --gtest_filter=AudioSessionEnumerator.*:AppLoopbackPid.*`
- [ ] Expected red state:
  - helpers/files do not exist.

## Task 4: Implement Session Enumeration and PID Helpers

- [ ] Implement `AudioSessionEnumerator`.
- [ ] Use `IMMDeviceEnumerator` -> default/render endpoint -> `IAudioSessionManager2` -> session enumerator.
- [ ] For each `IAudioSessionControl2`, call `GetProcessId`.
- [ ] Resolve PID to process name with Win32 process APIs.
- [ ] Sort/dedupe in a pure helper covered by tests.
- [ ] Implement PID parse helper for GUI.
- [ ] Run focused tests from Task 3.

## Task 5: Failing Tests for Process Loopback Backend Guardrails

- [ ] Add tests:
  - `ApplicationLoopbackCaptureStream.RejectsExclusiveInOpen`
  - `ApplicationLoopbackCaptureStream.RejectsZeroPid`
  - a compile/link test proving the class is part of `WinAudioCore`.
- [ ] Run:
  - `.\build\bin\Debug\WinAudioTests.exe --gtest_filter=ApplicationLoopbackCaptureStream.*`
- [ ] Expected red state:
  - class/open behavior missing.

## Task 6: Implement Process Loopback Capture Backend

- [ ] Implement async activation helper around `ActivateAudioInterfaceAsync`.
- [ ] Activate `IAudioClient` with `VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK`.
- [ ] Fill `AUDIOCLIENT_ACTIVATION_PARAMS` with include-process-tree mode and target PID.
- [ ] Initialize event-driven shared capture, set event handle, get `IAudioCaptureClient`, and reuse capture loop behavior.
- [ ] Return clear runtime errors for unsupported OS/SDK/API failures.
- [ ] Run focused backend tests.

## Task 7: Failing GUI Tests / Text Gates

- [ ] Add or extend GUI text/smoke coverage so it expects:
  - `Application Loopback` tab text.
  - `Refresh` button text.
  - editable PID field label.
  - list row semantics are accessible through static strings.
- [ ] Run the focused GUI/text test command already used by this repo if present; otherwise build `WinAudioGui`.
- [ ] Expected red state:
  - strings/page are not present.

## Task 8: Implement GUI Application Loopback Tab

- [ ] Add third tab.
- [ ] Add `wa::MonitorEngine appLoopback_`, status, visual state, session rows, selected row, PID buffer, started flag.
- [ ] Auto-refresh once when the tab first draws.
- [ ] Refresh button updates rows without overwriting manual PID input.
- [ ] Clicking a row copies the row PID into the PID input field.
- [ ] Start parses the PID input and calls `appLoopback_.start(WasapiShared, CaptureSource{ApplicationLoopback, ..., pid}, L"", 0, false)`.
- [ ] Stop shuts only application loopback.
- [ ] Reuse `drawChartPanel(0, appLoopback_, appLoopbackMs_, appLoopbackViz_)`.

## Task 9: Full Verification and Agent Review

- [ ] Run focused tests after each task.
- [ ] Run:
  - `.\test.bat Debug`
  - `cmake --build build --config Debug --target WinAudioGui -j`
- [ ] Run GUI smoke if available:
  - `tools\run_gui_smoke.ps1 -Config Debug -BuildDir build`
- [ ] Dispatch spec-compliance reviewer agent.
- [ ] Dispatch code-quality reviewer agent.
- [ ] Fix Critical/Important findings.
