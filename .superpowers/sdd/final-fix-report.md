# Final Fix Report — WinAudio feat/winaudio-mvp

## Status: DONE

---

## Files Modified

### `src/core/WasapiShared.h`
- Added `#include <condition_variable>` and `#include <mutex>` at the top.
- Added four private members to **both** `WasapiSharedCapture` and `WasapiSharedRender`:
  ```cpp
  std::mutex              readyMtx_;
  std::condition_variable readyCv_;
  bool                    ready_ = false;
  Result                  startResult_ = Result::Ok();
  ```

### `src/core/WasapiShared.cpp`

#### `WasapiSharedCapture::start()` — readiness handshake added
- Resets `ready_ = false` under `readyMtx_` before launching thread.
- After launching thread, blocks on `readyCv_.wait(lk, [this]{ return ready_; })`.
- Returns `startResult_` from the worker; calls `stop()` if it failed.

#### `WasapiSharedCapture::threadMain()` — all init exit paths converted
`signalReady` lambda introduced at top. Every early-return in the init section now calls `signalReady(...)` exactly once:

| Exit path | Signal call |
|-----------|-------------|
| CoCreateInstance FAILED | `signalReady(HrToResult(hr, "WasapiSharedCapture: CoCreateInstance"))` |
| GetDefaultAudioEndpoint FAILED | `signalReady(HrToResult(hr, "WasapiSharedCapture: GetDefaultAudioEndpoint"))` |
| Activate FAILED | `signalReady(HrToResult(hr, "WasapiSharedCapture: Activate"))` |
| GetMixFormat FAILED | `signalReady(HrToResult(hr, "WasapiSharedCapture: GetMixFormat"))` |
| GetMixFormat returned null | `signalReady(Result::Fail(-1, "WasapiSharedCapture: GetMixFormat returned null"))` |
| Initialize FAILED | `signalReady(HrToResult(hr, "WasapiSharedCapture: Initialize"))` |
| GetBufferSize FAILED | `signalReady(HrToResult(hr, "WasapiSharedCapture: GetBufferSize"))` |
| SetEventHandle FAILED | `signalReady(HrToResult(hr, "WasapiSharedCapture: SetEventHandle"))` |
| GetService FAILED | `signalReady(HrToResult(hr, "WasapiSharedCapture: GetService"))` |
| Start FAILED | `signalReady(HrToResult(hr, "WasapiSharedCapture: Start"))` |
| **Init succeeded** | `signalReady(Result::Ok())` — placed just before the capture loop |

`actualFormat_` and `bufferFrames_` are set before the success signal.

#### `WasapiSharedRender::start()` — same pattern as Capture

#### `WasapiSharedRender::threadMain()` — all init exit paths converted

| Exit path | Signal call |
|-----------|-------------|
| CoCreateInstance FAILED | `signalReady(HrToResult(hr, "WasapiSharedRender: CoCreateInstance"))` |
| GetDefaultAudioEndpoint FAILED | `signalReady(HrToResult(hr, "WasapiSharedRender: GetDefaultAudioEndpoint"))` |
| Activate FAILED | `signalReady(HrToResult(hr, "WasapiSharedRender: Activate"))` |
| GetMixFormat FAILED | `signalReady(HrToResult(hr, "WasapiSharedRender: GetMixFormat"))` |
| GetMixFormat returned null | `signalReady(Result::Fail(-1, "WasapiSharedRender: GetMixFormat returned null"))` |
| Initialize FAILED | `signalReady(HrToResult(hr, "WasapiSharedRender: Initialize"))` |
| GetBufferSize FAILED | `signalReady(HrToResult(hr, "WasapiSharedRender: GetBufferSize"))` |
| SetEventHandle FAILED | `signalReady(HrToResult(hr, "WasapiSharedRender: SetEventHandle"))` |
| GetService FAILED | `signalReady(HrToResult(hr, "WasapiSharedRender: GetService"))` |
| Start FAILED | `signalReady(HrToResult(hr, "WasapiSharedRender: Start"))` |
| **Init succeeded** | `signalReady(Result::Ok())` — placed after pre-roll + Start, just before the render loop |

Pre-roll silence buffer is performed before the `Start()` call, so it is complete before `signalReady(Ok)` fires.

### `src/core/Engine.cpp`

1. **`startCapture`**: Wrapped entire body (after `stop()`) in `try { ... } catch (const std::exception& e) { stop(); set Error state; return Result::Fail(-1, e.what()); }`. No-throw guarantee across public API.

2. **`startPlayback`**: Same try/catch wrapper. Additionally, `backend_->start()` call was moved out of `playbackLoop` and into `startPlayback` via its existing call in `playbackLoop`.

3. **`playbackLoop`**: The bare `backend_->start()` call is now:
   ```cpp
   Result r = backend_->start();
   if (!r) {
       std::lock_guard<std::mutex> lk(mtx_);
       status_.state = EngineState::Error;
       status_.message = r.message;
       running_.store(false);
       return;
   }
   ```
   Render-init failure now surfaces to the CLI poll loop immediately and stops the engine.

### `CLAUDE.md` (Fix 3 — doc corrections)
- `静纯 C++` → `纯 C++`
- `Result<T>` → `Result` (not templated) — fixed in both architecture section occurrences
- `WasapiBackend` → `WasapiSharedCapture / WasapiSharedRender`
- `pollSnapshot()` reference removed from RingBuffer description

---

## Build Result
**0 errors, 0 warnings** — all four targets (WinAudioCore, WinAudioCli, WinAudioGui, WinAudioTests) built successfully under `/W4`.

## Unit Test Result
**14 PASSED** — `WinAudioTests.exe` passed all 14 tests across Smoke, AudioFormat, RingBuffer, and WavFile suites.

## Hardware Proof (Critical Bug Fixed)

Device mix format (from `WinAudioCli.exe list --capture`, default device `*`):
```
??? (A4tech USB2.0 Camera (Audio))  [48000 Hz 1 ch float]
```

WAV header from `cap2.wav` (3-second capture):
| Field | Offset | Value | Expected |
|-------|--------|-------|----------|
| wFormatTag | 20 | **3** (IEEE_FLOAT) | 3 (float) ✓ |
| nChannels | 22 | **1** | 1 ✓ |
| nSamplesPerSec | 24 | **48000** | 48000 ✓ |
| wBitsPerSample | 34 | **32** | 32 ✓ |

Raw bytes [20..35]: `03 00 01 00 80 BB 00 00 00 EE 02 00 04 00 20 00`

**Before the fix**, `start()` returned immediately before the worker ran `GetMixFormat`, so `actualFormat_` was still `AudioFormat{}` (48000/2/16/PCM — the default zero-initialized struct), causing the WAV header to record 16-bit PCM while the ring carried 32-bit float samples.

**After the fix**, `start()` blocks on the condition variable until the worker completes all device init, so `actualFormat_` is valid when `captureLoop` reads it.

## Playback Result
`WinAudioCli.exe play --in cap2.wav` played through and exited with `done` — clean exit, no hang.
