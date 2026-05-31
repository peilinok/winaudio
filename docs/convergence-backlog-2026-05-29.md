# WinAudio Convergence Backlog

Date: 2026-05-29

Context:
- Use together with:
  - `docs/convergence-audit-2026-05-29.md`
  - `tools/run_convergence_check.ps1`

This file is the prioritized residual-issues list after the current convergence pass.

## Must Fix

### 1. Stronger GUI interaction verification

Why:
- GUI interaction has improved structurally, but current evidence is still mostly smoke-level:
  - process starts
  - probe busy state appears
  - probe restores
- There is still no exhaustive GUI automation for every transition path, but the busy-cycle control-disable/restore surface is now much broader than before and covers most of the configuration controls directly, including quick/matrix mutual exclusion, running-session probe restore, and refresh-while-running preservation.
- The opt-in `gui_smoke_test` registration path is now exercised end-to-end successfully, so GUI coverage is no longer just an ad hoc script asset; the remaining gap is breadth, not absence of a formal project test target.
- A dedicated wrapper now exists to reproduce that formal GUI target path without manually reconstructing configure/build/ctest steps:
  - `tools/run_gui_smoke_ctest.ps1`
- The lighter formal CLI integration target now also has a matching project-level wrapper:
  - `tools/run_cli_integration_ctest.ps1`

Impact:
- interaction
- stability confidence

Next action:
- continue expanding high-value interaction checks where the remaining gap is still mostly semantic or transition-oriented rather than simple control availability

### 2. Clear separation of logic tests vs. real-hardware validation

Why:
- This improved a lot already, but real hardware verification still lives partly in CLI runs and partly in expectations around probe text semantics.
- The intended architecture should be explicit:
  - fast deterministic tests
  - explicit real-hardware checks
- The opt-in `hardware_validation_test` registration path is now exercised end-to-end successfully, so the remaining gap is no longer lack of a formal project target but the fact that it still remains intentionally environment-coupled and outside the lightweight default baseline.
- A durable output artifact now also exists for that hardware path:
  - `build-hwtest/<Config>/hardware_validation_artifacts/hardware_validation_output.txt`

Impact:
- stability
- maintenance

Next action:
- keep the dedicated integration-style hardware layer current and only expand it where it closes real hardware blind spots

## Should Fix

### 3. Devices mode friendliness

Why:
- `devices` output is now stable and complete.
- Non-ASCII friendly names are escaped as `\uXXXX`, which is correct and robust, but not ideal for readability.

Impact:
- usability

Next action:
- decide whether to keep escaped output as default
- or optionally provide a more human-readable alternative mode/output format

### 4. Matrix CLI coverage breadth

Why:
- CLI currently controls `matrix-source`, but not more granular matrix dimensions.
- Quick probe has significantly richer CLI control than matrix.

Impact:
- usability
- test coverage ergonomics

Next action:
- if needed, expose one or two high-value matrix CLI scoping options beyond source

### 5. Quick summary semantics for additional edge cases

Why:
- Quick summary is much better now, but edge-case wording can still continue to improve.
- Example areas:
  - start-failure wording
  - monitor-off wording
  - tick-failure wording consistency

Impact:
- usability

Next action:
- continue only if real user confusion appears

## Could Fix

### 6. Pretty-print / export options for CLI outputs

Why:
- Current CLI is highly usable, but still console-oriented.
- JSON or machine-readable output could help future automation.

Impact:
- usability

Next action:
- defer unless automation demand becomes concrete

### 7. Extra matrix summary dimensions

Why:
- Matrix already has many summary layers:
  - backend
  - pair
  - profile
  - source
  - align
  - delay
  - buffer
- More can be added, but returns are diminishing.

Impact:
- usability

Next action:
- only add if a real debugging blind spot remains

## Recommended Next Step

If continuing immediately, prefer this order:

1. strengthen GUI interaction verification
2. formalize real-hardware validation separation
3. only then consider more CLI or matrix surface-area expansion
