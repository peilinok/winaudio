# WinAudio GUI Manual Verification Checklist

Date: 2026-05-29

Use this together with:
- `tools/run_convergence_check.ps1`
- `docs/convergence-audit-2026-05-29.md`

This checklist is intentionally lightweight. It focuses on the highest-value GUI behaviors that still benefit from direct human confirmation beyond the current smoke script.

## 1. Launch and idle state

Verify:
- the window opens successfully and stays alive
- `Start Session` is enabled while idle
- `Stop` is disabled while idle
- the title shows the current session state instead of a stale probe state

## 2. Source mode remap

Script coverage:
- `tools/run_convergence_check.ps1` now checks the loopback capture-device label, the `Loopback capture devices` summary line, and the capture combo's full remap to the render-endpoint list directly.

Set `Source Mode` to `System Loopback`.

Verify:
- the capture-device label visually reads `Loopback Capture Device`
- the summary line visually uses `Loopback capture devices`
- the capture-device combo still looks visually correct while presenting loopback-backed render endpoints rather than microphone endpoints

Set `Source Mode` back to `Microphone`.

Verify:
- the capture-device label returns to `Capture Device`
- the capture-device combo returns to microphone/capture endpoints

## 3. Follow defaults

Script coverage:
- `tools/run_convergence_check.ps1` now checks the follow-defaults + loopback summary and diagnostics text directly.
- `tools/run_convergence_check.ps1` now also checks both sides of default-device-change behavior during an active session:
  - with `Follow default devices` off, a default-device-change notification is tracked explicitly as `Last device change: default-device-change => tracked-no-rebuild`
  - with `Follow default devices` on, the session survives the notification and surfaces both:
    - `Last device change: default-device-change => rebuild-success`
    - `Last rebuild: default-device-change => success`

Turn `Follow default devices` on.

Verify:
- capture and render device selectors become unavailable immediately
- the summary still looks visually correct while explaining that manual device picks are inactive
- if a default-device change happens while the session is running, the session still recovers cleanly and the diagnostics still look coherent

Turn it back off.

Verify:
- device selectors become available again

## 4. Monitor off

Script coverage:
- `tools/run_convergence_check.ps1` now checks the monitor-off summary and diagnostics text directly.

Turn `Monitor Playback` off.

Verify:
- render-pipeline controls become unavailable immediately
- the summary still looks visually correct while explaining that the render pipeline is disabled for monitoring

Turn it back on.

Verify:
- render-pipeline controls become available again, subject to auto-align state

## 5. Auto-align

Turn `Render Auto Align` on.

Verify:
- render format selectors become unavailable immediately
- the explanatory note about render format following capture remains visible

## 6. Running-session config drift

Script coverage:
- `tools/run_convergence_check.ps1` now checks a running-session capture-format edit directly.

Start a session, then change the capture sample-rate selection while it stays Running.

Verify:
- the diagnostics text updates `Current configured capture: ...` to the newly edited value
- the diagnostics text still preserves the prior `Active session requested capture: ...` for the already-active stream
- the session remains Running during the edit and can still stop cleanly afterward

## 7. Running-session monitor-off drift

Script coverage:
- `tools/run_convergence_check.ps1` now checks monitor-off while Running directly.

Start a session, then turn `Monitor Playback` off while it stays Running.

Verify:
- the summary explains that render monitoring is turned off for the next rebuilt or restarted session
- the diagnostics still preserve the already-active render session details instead of claiming the active render pipeline is already disabled
- the session remains Running during the edit and can still stop cleanly afterward

Turn it back off.

Verify:
- render format selectors become available again, unless monitor is off

## 6. Session buttons

Script coverage:
- `tools/run_convergence_check.ps1` now checks both button availability and the window title's session-state transition.

Click `Start Session`.

Verify:
- `Start Session` becomes unavailable
- `Stop` becomes available
- the title visually reflects `Running`
- clicking `Refresh Devices` while still `Running` does not collapse the session back to idle
- if `Follow default devices` is on while the session is `Running`, `Refresh Devices` also leaves coherent rebuild diagnostics instead of silently drifting the tracked device selection
- if `Follow default devices` is on while the session is `Running`, the capture/render selectors remain unavailable across the refresh-rebuild path
- while the session is `Running`, the UI text also makes it clear that ordinary configuration edits apply to the next rebuilt or restarted session, not the already-active stream

Click `Stop`.

Verify:
- `Start Session` becomes available again
- `Stop` becomes unavailable again

## 7. Quick probe busy cycle

Click `Run Quick Probe`.

Verify:
- title changes to `Quick Probe Running`
- quick-probe button text changes to the running label
- `Run Probe Matrix` also becomes unavailable while quick probe is busy
- if the app was idle before the quick probe started, `Stop` remains unavailable while the quick probe is busy
- key config controls become unavailable during the probe
- state restores after completion
- if the app session was already `Running` before the quick probe started:
  - it returns to `Running` after the probe completes
  - `Stop` remains available throughout that running-session probe path

## 8. Probe matrix busy cycle

Click `Run Probe Matrix`.

Verify:
- title changes to `Probe Matrix Running`
- matrix button text changes to the running label
- `Run Quick Probe` also becomes unavailable while probe matrix is busy
- if the app was idle before the matrix started, `Stop` remains unavailable while the matrix is busy
- key config controls become unavailable during the probe
- state restores after completion
- if the app session was already `Running` before the matrix started:
  - it returns to `Running` after the matrix completes
  - `Stop` remains available throughout that running-session matrix path

## 9. Result semantics spot-check

Script coverage:
- `tools/run_convergence_check.ps1` now checks quick-probe result semantics in the real GUI for both `Monitor Playback` on and off, including the detailed disabled render-wave/runtime fields and note.

Run one quick probe with:
- `Monitor Playback` on

Verify:
- probe text still looks visually correct while showing negotiated render details and non-disabled render semantics

Run one quick probe with:
- `Monitor Playback` off

Verify:
- probe text still looks visually correct while using render-disabled semantics instead of claiming an active render negotiation/match
- the disabled render-wave/runtime fields and explanatory note still look visually correct

## 10. Close while background work is active

Start a quick probe or probe matrix, then close the window before it completes.

Verify:
- the process exits cleanly
- no second window appears
- no obvious crash dialog or access-violation dialog appears
