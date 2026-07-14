<!--
PR title: Conventional Commits format, e.g. "feat(gui): add application loopback capture".
The title goes verbatim into GitHub Release notes (generate_release_notes) — keep it user-readable.
Full rules: CLAUDE.md, section "Commit 与 PR 规范".
-->

## What / Why

<!-- What changes and why. Link the issue (Closes #N) and/or the design doc under docs/superpowers/specs/. -->

## How

<!-- Key design decisions and trade-offs — a paragraph or two, not a diff walkthrough. -->

## Testing

<!-- What you ran and the results (paste test totals). For real-device behavior (latency, drift,
     exclusive-format negotiation), state the device(s) used and the smoke outcome. -->

## Checklist

- [ ] Commits follow Conventional Commits and are atomic (each builds and passes tests)
- [ ] `test.bat Debug` and `test.bat Release` pass locally
- [ ] New core logic has gtest coverage that runs without real audio devices
- [ ] GUI changes: screenshots attached
- [ ] Real-device behavior touched: manual smoke done (CLI `monitor` or GUI), result stated above
