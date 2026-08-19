# Stream init is a shared recipe, not share-mode subclasses

`prepareClient` mixed Shared mix/requested, Exclusive probe, align-retry, and hardcoded Initialize flags in one function, with a cloned Shared path in Application Loopback. We deepen **Stream init** as one callable recipe used by every Track and silent-render: mix vs requested conversion, exclusive probe and align-retry, duration, and stream flags. Direction subclasses (Capture / Render / System Loopback / silent-render) stay the one inheritance axis. Share mode is policy inside Stream init, not a second class hierarchy.

## Considered Options

- **Shared/Exclusive subclasses of WasapiStream** — rejected. Orthogonal to Capture source / direction, would duplicate the worker-thread scaffold, fails the deletion test.
- **Private `prepareShared` / `prepareExclusive` on WasapiStream** — rejected. Application Loopback is not a WasapiStream subclass; a private split would be extracted again.
- **Put the Application Loopback 44100 mix-fallback inside Stream init** — rejected. Ordinary Tracks would start silently degrading on mix failure. Fallback stays on the Application Loopback activate adapter as a second Stream init call with that format requested.
- **A new flags type beside Stream params** — rejected. GUI Advanced already writes Stream params; a parallel bag is a third channel. User-overridable flags go in Stream params. Loopback extras stay caller-supplied so Stream init does not inspect Capture source. `extraSharedInitFlags` is deleted.
- **Expose EVENTCALLBACK, NOPERSIST, RATEADJUST, or pull this round** — rejected. The pump is event-driven; EVENTCALLBACK is locked on. This round Stream params only gains AutoConvert (`Default` = today's rule; `Force`/`Off` decouple conversion from “has requested format”). Exclusive `Force` fails; no silent downgrade. `AUTOCONVERT` stays paired with `SRC_DEFAULT_QUALITY`.
- **Wire SetClientProperties / ducking into Application Loopback in the same change** — rejected. That path never made those calls. This round it only switches Initialize to Stream init (`bufferMs`, AutoConvert, caller `LOOPBACK`). Client properties and ducking stay on the WasapiStream scaffold.

## Consequences

- Stream init sits after endpoint or Application Loopback activation and before ducking / Start. It does not own the pump.
- Tests hit Stream init through an internal fake audio-client adapter (record/inject `GetMixFormat`, `IsFormatSupported`, `Initialize`, and one `AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED` rebuild). That seam is not `IAudioBackend`.
- GUI Advanced does not gain new checkboxes in this change. AutoConvert is expressible in Stream params so a later UI can write it.
- Reopens device-params spec §8 only for AutoConvert; remaining `AUDCLNT_STREAMFLAGS_*` stay out until a later Stream params field.
