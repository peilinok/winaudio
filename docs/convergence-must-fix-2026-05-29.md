# WinAudio Must-Fix Backlog

Date: 2026-05-29

Use together with:
- `docs/convergence-audit-2026-05-29.md`
- `docs/convergence-backlog-2026-05-29.md`
- `tools/run_convergence_check.ps1`

This file contains only the highest-priority remaining convergence items.

## 1. Stronger GUI interaction verification

Status:
- partially covered

Current evidence:
- GUI smoke now verifies:
  - process starts
  - probe title busy state appears
  - probe button text changes
  - probe button disables
  - start button disables
  - auto-align toggles render-format control availability
  - follow-defaults toggles device selector availability
  - start/stop availability flips with session state
  - state restores
- that GUI smoke now also has a dedicated reusable entrypoint:
  - `tools/run_gui_smoke.ps1`
- and an opt-in formal test registration path:
  - `gui_smoke_test`
- default-device-change behavior in running sessions is now covered on both sides:
  - explicit `tracked-no-rebuild` diagnostics in manual-device mode
  - explicit `rebuild-success` plus `Last rebuild` diagnostics in follow-defaults mode
- a lightweight manual checklist now exists:
  - `docs/gui-manual-verification-checklist-2026-05-29.md`

Remaining gap:
- still smoke-level only
- not yet a full repeatable verification of all critical controls and transitions

Why must-fix:
- interaction regressions are still easier to miss than CLI regressions

## 2. Build environment hardening

Status:
- improved locally, not fundamentally solved

Current evidence:
- the machine can hit an MSBuild failure caused by simultaneous `Path` and `PATH`
- local wrapper `tools/invoke_msbuild_safe.ps1` now mitigates this for workspace builds
- that wrapper now also retries once on linker `LNK1168` for a narrow allowlist of known workspace binaries by terminating only the stale local process that is blocking the output exe
- MSVC builds now also compile with `/FS`, which reduces shared `winaudio_core.pdb` write contention during concurrent builds
- explicit inspector `tools/inspect_build_environment.ps1` now reports whether the current process contains the collision
- convergence and hardware-validation scripts now print environment state before building

Remaining gap:
- the machine-level root cause still exists
- direct builds outside the wrapper can still fail intermittently because the `Path/PATH` duplication itself is still external to the repo

Why must-fix:
- this undermines trust in any future convergence work if forgotten or bypassed

## 3. Explicit real-hardware validation layer

Status:
- partially covered

Current evidence:
- CLI probe runs are now the main real-hardware evidence source
- logic/text tests are mostly stub-backed and fast
- dedicated script now exists:
  - `tools/run_hardware_validation.ps1`
  - `tools/test_cli_integration.ps1`
- those scripts already behave partly like an integration-style layer by chaining:
  - device discovery
  - explicit device-id reuse
  - focused quick/matrix runs

Remaining gap:
- the dedicated hardware validation flow now has an opt-in formal test-target registration path, but it is still intentionally kept out of the default lightweight `ctest` baseline because it remains environment-coupled to real hardware
- real-hardware validation is therefore closer to a first-class test target than before, but still not an always-on baseline guarantee

Why must-fix:
- keeps long-term stability work honest
- prevents slow drift between what tests guarantee and what real hardware actually does

## 4. Device discovery UX tradeoff review

Status:
- improved, but not settled

Current evidence:
- `devices` output is stable
- non-ASCII friendly names are preserved as `\uXXXX`
- an opt-in native-name mode now exists:
  - `--device-name-format=native`

Remaining gap:
- default output is still optimized for stability and machine parsing rather than human readability
- we still need to decide whether escaped output should remain the default long-term, even though a native-name mode now exists

Why must-fix:
- device discovery is the front door to targeted validation

## 5. Matrix CLI scoping beyond source

Status:
- partially covered

Current evidence:
- `--matrix-source=loopback` now genuinely removes microphone rows
- `--matrix-render-backend=wasapi|wave|both` narrows matrix output by render backend
- `--matrix-capture-backend=wasapi|wave|both` narrows matrix output by capture backend
- `--matrix-align=on|off|both` narrows matrix output by auto-align state
- `--matrix-profile=pcm16-48k-stereo|pcm24-44k-mono|both` narrows matrix output by format profile
- `--matrix-wasapi-share=shared|exclusive|both` narrows matrix output by WASAPI share mode
- `--matrix-delay=0ms|120ms|both` now narrows matrix output by fixed-delay profile
- `--matrix-buffer=cap40-ren40|cap80-ren120|both` now narrows matrix output by capture/render buffer pair
- convergence and hardware-validation scripts now include multiple focused loopback matrix views

Remaining gap:
- scoping is no longer limited to source, and now also covers delay/buffer, but the views are still fixed and hand-curated rather than a more general targeting layer

Why must-fix:
- without better scoping, matrix can still be too broad for fast targeted diagnosis

## Recommended Order

1. stronger GUI interaction verification
2. build environment hardening
3. explicit real-hardware validation layer
4. decide final device discovery UX
5. improve targeting on top of the current scoped matrix views instead of expanding option count by default
