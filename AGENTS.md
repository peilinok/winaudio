# AGENTS.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

WinAudio is a **Windows-only** C++20 audio link verification/diagnostics project. It builds two entrypoints over one shared core library: `winaudio` (Win32 GUI) and `winaudio_probe` (CLI). Requires MSVC (VS 2022), CMake ≥ 3.24, Windows SDK, and PowerShell. It links Windows audio/media libs directly (`ole32`, `mmdevapi`, `winmm`, `avrt`, `Mfplat`, `Mf`, `Mfcore`), so it does not build or test off Windows.

## Build, Test, Run

```powershell
cmake -S . -B build
cmake --build build --config Debug          # builds winaudio, winaudio_probe, and all *_test targets
ctest --test-dir build -C Debug --output-on-failure
.\build\Debug\winaudio.exe                  # GUI
.\build\Debug\winaudio_probe.exe --help     # CLI
```

- **If a normal build hits `LNK1168` (output file locked) or a `PATH`/`Path` env collision**, use the safe wrapper instead of raw `cmake --build`:
  `powershell -ExecutionPolicy Bypass -File tools\invoke_msbuild_safe.ps1 -PrintEnvironmentSummary cmake --build build --config Debug --target winaudio winaudio_probe`
- **Run a single unit test**: each test is a standalone exe linked to `winaudio_core` (no GoogleTest) — run `.\build\Debug\session_controller_test.exe` directly, or `ctest --test-dir build -C Debug -R session_controller_test --output-on-failure`.
- **Reproduce the CI baseline locally** (the exact set that gates PRs):
  `powershell -ExecutionPolicy Bypass -File tools\run_hosted_stable_ctest.ps1 -Config Release -BuildDir build-ci`
- **One-shot convergence check** (build + CLI help + devices + quick + matrix + integration + GUI smoke + CTest):
  `powershell -ExecutionPolicy Bypass -File tools\run_convergence_check.ps1 -Config Debug -BuildDir build`

PowerShell scripts in `tools/` are first-class validation entrypoints, not throwaway helpers — when adding a build/validation path, align CMake + CTest + `tools/*.ps1` together.

## Architecture

Read `docs/architecture.md` for the authoritative, maintained layering doc. The essentials:

- **`winaudio_core`** (CMake static lib) holds all behavior: `src/audio/*`, `src/app/*`, `src/rtc/*`. GUI, CLI, and tests all link it. **Keep behavior and semantics inside core; entrypoints stay thin.** Do not scatter logic into `win_audio_app.cpp` or `probe_cli_main.cpp`.
- **`AppModel`** (`src/app/app_model.*`) is the shared brain: config state, device refresh, probe/matrix orchestration, cached text snapshots, bridging the controller to both GUI and CLI. It does state + caching, **not** long text assembly.
- **`AudioSessionController`** (`src/audio/audio_session_controller.*`) owns *only the main audio path*: capture → analyzer/stats/waveform → optional resampler/ring buffer → render monitor → dump. `Start()`/`Tick()` success is determined **solely by the main path**.
- **`RtcSidecar`** (`src/audio/rtc_sidecar.*`) → `AgoraRtcPublisher` (`src/rtc/agora_rtc_publisher.*`) is an **optional bypass publisher** ("local capture → Agora RTC"), not a third audio backend. The controller talks to it through a few methods only.
- **`probe_ui_text`** (`src/app/probe_ui_text.*`) is the single home for GUI/CLI shared strings, labels, button text, capability descriptions, and RTC status text. Reuse its builders; never re-assemble RTC/status text inline in the GUI or CLI.
- Backends/pipeline live under `src/audio/backends/*` (WASAPI + Wave adapters, device enum, `wave_format_utils`), `src/audio/pipeline/*` (ring buffer, signal analyzer, WAV dump), `src/audio/resample/*`.

### Non-negotiable invariants (these are the point of the current design)

- **RTC never controls the main path.** If RTC is unavailable, fails to join, or fails to publish mid-run, the sidecar deactivates itself and records only RTC state/log — `quick`/`matrix`/`capture-open`/GUI sessions still succeed on the main path alone. The *only* mode that returns non-zero on RTC failure is `winaudio_probe rtc` (the dedicated RTC health check).
- **Error attribution is partitioned.** Main-path errors go to `SessionRuntimeStats.last_error_stage`/`last_error_message`. RTC errors go to `AgoraRtcStats.last_error_*`, `rtc_text()`, and logs — never to the main `last_error_*`.
- **GUI/CLI contract parity.** `winaudio` and `winaudio_probe` must use identical terminology, state names, device labels, and failure stages for shared concepts.
- `monitor=off` disables the render pipeline and `--render-device-id` is ignored. `System Loopback` may force monitor playback off at the main-path level to avoid a loopback storm. Loopback capture device IDs come from `devices --source=loopback`, not the mic device list.

## CLI shape

`winaudio_probe <mode>` where mode ∈ `quick` | `matrix` | `devices` | `capture-open` | `rtc`. Sources: `--source=mic|loopback|app-loopback` (app-loopback needs `--app-loopback-process=<name-or-pid>`). Option parsing lives in `ParseProbeCliOptions` (`src/app/probe_cli_options.cpp`); `ProbeCliOptions` wraps a `SessionConfiguration`. `--rtc-token` is never echoed in output.

## Agora SDK (optional, off by default)

- Real SDK integration is gated by `-DWINAUDIO_ENABLE_AGORA_SDK=ON`. Default builds compile the RTC abstraction but stub the runtime.
- SDK source resolves in priority order: `WINAUDIO_AGORA_SDK_URL` env > `.env` file > `WINAUDIO_AGORA_SDK_ROOT` env pointing at a local `Agora Native SDK for Windows` `sdk/` dir.
- At runtime the SDK is dynamically loaded; a missing `agora_rtc_sdk.dll` downgrades RTC to disabled (with a shown reason) rather than failing.
- **Never commit `.env`, Agora tokens, private SDK URLs, or machine-specific device IDs.**
- **RTC tests must inject `AgoraRtcPublisherFactory` fakes** (runtime-unavailable / join-success / publish-failure scenarios). Do not condition tests on `WINAUDIO_ENABLE_AGORA_SDK` or the presence of a local DLL.

## Test layers & test-first expectation

The project follows a test-first / contract-stable governance. Any change to core audio logic, CLI parsing, GUI state semantics, diagnostic text, or device discovery should update the test/script/checklist **before** the implementation lands. Map your change to the layer that must be checked:

| Change area | Must check |
| --- | --- |
| Main-path lifecycle / error boundaries | `session_controller_test` |
| CLI parsing & exit codes | `probe_cli_test` |
| GUI/CLI text, RTC text, capability text | `app_model_text_test`, `probe_ui_text_test` |
| Data pipeline / resampling / waveform | `core_pipeline_test` |
| Build or validation scripts | `build_environment_tools_test`, `convergence_helpers_test` |
| Real device discovery / desktop paths | `cli_integration_test`, `gui_smoke_test`, `hardware_validation_test` |

- **Hosted-stable baseline** (gates PRs, no real-hardware dependency): the six `*_test` unit exes plus `build_environment_tools_test` and `convergence_helpers_test`.
- **Environment-dependent, NOT in PR CI**: `cli_integration_test` (needs enumerable devices/loopback), and the opt-in `gui_smoke_test` / `hardware_validation_test` (enable via `-DWINAUDIO_ENABLE_GUI_SMOKE_TESTS=ON` / `-DWINAUDIO_ENABLE_HARDWARE_VALIDATION_TESTS=ON`). Keep real-device dependencies out of the default baseline.

Real audio negotiation results vary by machine/driver/default device — tests assert **semantic stability** (output fields, failure stages, diagnostic text), not byte-identical device output.

## Conventions

- C++20, 2-space indent, same-line braces, small focused helpers. Types `PascalCase`; functions `camelCase`/verb phrases. Files lowercase-with-underscores (e.g. `audio_session_controller.cpp`).
- Commits: short imperative summaries (e.g. `Decouple RTC sidecar from main audio session`), one logical change each. PRs state user-visible behavior, affected validation layers, and any Windows/device assumptions.
- **Any user-visible semantic change must also update README, `probe_ui_text` tests, and any relevant script assertions / manual checklist** — code-only changes to visible behavior are out of contract.
- Note: Ignore behavior is defined by `.gitignore` (source of truth): `docs/*` is ignored except `docs/architecture.md`; `build/`, `artifacts/`, `third_party/agora-sdk/`, and `.env` are also ignored.
