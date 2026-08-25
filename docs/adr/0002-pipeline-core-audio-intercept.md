# Pipeline Inspector intercepts Core Audio COM in the target app, not audiodg

The Inspector must show another process's Initialize arguments, which public session APIs cannot. We intercept Core Audio COM in-process on On-demand attach (same bitness, fail closed without debug rights). We do not hook audiodg APO COM, do not clone system-wide API Monitor, and do not require an always-elevated GUI. Pre-attach Initialize is filled from ETW / endpoint graph / Shared probe; the Call log keeps control-path calls and optional pump metadata in a small ring.

## Considered Options

- **Public APIs only** — rejected. Cannot observe the target's `WAVEFORMATEX`, RAW, or category as Observed.
- **Inject audiodg for `IAudioProcessingObject`** — rejected for v1. Protected process; still no supported way to dump APO internals safely.
- **Hook all Win32/COM or XAudio2/DirectSound/WinRT entry points** — rejected. Out of the Core Audio intercept closed set.
- **Auto-inject every audio process, or launch-suspended monitor** — rejected. On-demand attach only.
