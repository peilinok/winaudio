# What `RegisterSessionNotification` reports for a new Live session

Date: 2026-09-29.
Ticket: [peilinok/winaudio#107](https://github.com/peilinok/winaudio/issues/107).
Parent map: [Pipeline Live session watch spec](https://github.com/peilinok/winaudio/issues/105).

A Live session is a Windows mixer row (one audio session on one endpoint for one process). It is not a Track.

This note is API fact from Microsoft Learn Win32 pages and the Windows SDK headers those pages name. The 2026-08-21 Pipeline Inspector design named `IAudioSessionNotification` for Live session watch; that is product context, not proof of API behaviour. This note does not implement a watcher and does not recommend which API Pipeline should use.

SDK headers read on this machine: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\audiopolicy.h`, `audioclient.h`, and `AudioSessionTypes.h`.

## Short answer

`IAudioSessionManager2::RegisterSessionNotification` registers an app-implemented `IAudioSessionNotification`. After that, and only after `IAudioSessionEnumerator::GetCount` has been called, the audio engine calls `OnSessionCreated(IAudioSessionControl*)` when a **new** session is created on the **one endpoint** whose `IMMDevice` was used to activate that manager. The callback is create-only. Sessions that already existed are not delivered this way; an enumerate pass is still required. Destroy, expire, and Inactive are not on this interface.

The pointer is `IAudioSessionControl`. PID and session instance id are on `IAudioSessionControl2` after `QueryInterface`. State and display name are on `IAudioSessionControl`. Device id and data flow are not on the control; they come from the endpoint that owns the manager. Volume and mute are not methods of `IAudioSessionControl` / `IAudioSessionControl2`.

Relative to WASAPI: a session is opened when the first stream is assigned with `IAudioClient::Initialize`. `OnSessionCreated` is documented as firing when that new session is created / "activated on the device endpoint". It is not documented as waiting for `IAudioClient::Start`. After `Initialize`, the session state pages say the session is Inactive until a stream is running (`Start`).

## 1. What the registration reports

[`IAudioSessionManager2::RegisterSessionNotification`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registersessionnotification) "registers the application to receive a notification when a session is created." The argument is the application's `IAudioSessionNotification`. On success the manager `AddRef`s that interface.

[`IAudioSessionNotification`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionnotification) "provides notification when an audio session is created." In `audiopolicy.h` (SDK 10.0.26100.0) the interface inherits `IUnknown` and declares a single method:

```cpp
virtual HRESULT STDMETHODCALLTYPE OnSessionCreated(
    /* [in] */ IAudioSessionControl *NewSession) = 0;
```

[`OnSessionCreated`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionnotification-onsessioncreated) "notifies the registered processes that the audio session has been created." `NewSession` is "the `IAudioSessionControl` interface of the audio session that was created."

[`UnregisterSessionNotification`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-unregistersessionnotification) "deletes the registration to receive a notification when a session is created." Pass the same pointer used to register. On success the manager `Release`s it.

The header matches: `IAudioSessionManager2` declares `RegisterSessionNotification` / `UnregisterSessionNotification` taking `IAudioSessionNotification*`, and nothing else on that pair.

## 2. When `OnSessionCreated` fires relative to `Initialize` / `Start`

### Session creation is `Initialize`, not `Start`

[`IAudioClient::Initialize`](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize) takes `AudioSessionGuid`. "If the GUID identifies a session that has been previously opened, the method adds the stream to that session. If the GUID does not identify an existing session, the method opens a new session and adds the stream to that session." `NULL` is `GUID_NULL`.

The [Audio Sessions](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-sessions) topic: "When a client initializes an audio stream, it assigns the audio stream to an audio session." "A client assigns an audio stream to a particular session at the time that it initializes the stream object."

[`IAudioSessionControl::GetState`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-getstate): "The client creates a stream by calling the `IAudioClient::Initialize` method. At the time that it creates a stream, the client assigns the stream to a session. A session begins when a client assigns the first stream to the session. Initially, the session is in the inactive state. The session state changes to active when the first stream in the session begins running."

[`AudioSessionState`](https://learn.microsoft.com/en-us/windows/win32/api/audiosessiontypes/ne-audiosessiontypes-audiosessionstate): "When a client opens a session by assigning the first stream to the session (by calling the `IAudioClient::Initialize` method), the initial session state is inactive. The session state changes from inactive to active when a stream in the session begins running (because the client has called the `IAudioClient::Start` method)."

[`IAudioClient::Start`](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-start) "starts the audio stream" and requires a prior successful `Initialize`. It does not create a session.

So on these pages, **opening / creating the session is `Initialize` of the first stream**. **`Start` is what makes a stream running**, which those same pages treat as Inactive → Active.

### What `OnSessionCreated` itself says

`OnSessionCreated` remarks: "The audio engine calls **OnSessionCreated** when a new session is activated on the device endpoint." It does not mention `IAudioClient::Start`. Register / Unregister / the interface page all say "when a session is created."

The docs do not give a call-stack order such as "before `Initialize` returns" or "after `Start`." Combining the create wording with the session-lifetime pages, the documented trigger is **new session creation on that endpoint**, which is the first-stream `Initialize`, not `Start`. Whether the callback can race `Initialize`'s return is not stated.

`OnSessionCreated` is "called from the session manager thread." If the implementation wants to keep `NewSession` after the call returns, it "must take a reference to the session."

### A wording clash on Active (not on create)

[`IAudioSessionEvents::OnStateChanged`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionevents-onstatechanged) remarks say the system "changes the state of a session from inactive to active at the time that a client opens the first stream in the session" via `Initialize`, and "from active to inactive" when the last `IAudioClient` is released. That disagrees with `AudioSessionState` and `GetState`, which put Active at `Start` and Expired at last-stream release. This note does not pick a winner. It only needs the create-vs-Start split, which Register / OnSessionCreated / Initialize / AudioSessionState agree on: **create is not Start**.

## 3. Capture versus render

[`IAudioSessionManager2`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionmanager2) is activated on **one** `IMMDevice`: obtain the endpoint, then `IMMDevice::Activate(..., IID_IAudioSessionManager2, ...)`. Tasks listed: ducking notifications, "a notification when a session is created," and "Enumerate sessions **on the audio device that was used to get the interface pointer**."

The same page's sample calls `GetDefaultAudioEndpoint(eRender, eConsole, ...)`. That is sample code for a default render device, not a statement that capture is excluded.

[Audio Sessions](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-sessions): "An audio session contains either rendering streams or capture streams, but not both." "Each session is associated with only one audio endpoint device." "A session can never contain both capture and rendering streams because a capture stream can be associated only with a capture device and a rendering stream can be associated only with a rendering device." Loopback sessions are treated like capture for volume persistence.

`IAudioSessionControl` has no data-flow getter (`audiopolicy.h`). Flow is the flow of the endpoint the manager was activated on.

Therefore: **the same create notification exists for capture and for render**, but **each registration is per endpoint**. A manager on a capture `IMMDevice` reports sessions created on that capture endpoint. A manager on a render `IMMDevice` reports sessions on that render endpoint. Covering both mixer-row directions means activating a manager (and registering) on each capture and each render endpoint of interest. The API does not say "render only."

[`IAudioSessionControl`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessioncontrol): "An audio session is a collection of shared-mode streams. This interface does not work with exclusive-mode streams." [`ISimpleAudioVolume`](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-isimpleaudiovolume) says the same. Exclusive streams are outside this control surface.

## 4. What the `IAudioSessionControl` at callback time can supply

`OnSessionCreated` passes `IAudioSessionControl*` only. [`IAudioSessionControl2`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessioncontrol2) is obtained by `IAudioSessionControl::QueryInterface`. The header has `IAudioSessionControl2 : public IAudioSessionControl` with `GetSessionIdentifier`, `GetSessionInstanceIdentifier`, `GetProcessId`, `IsSystemSoundsSession`, `SetDuckingPreference`.

Microsoft does not list extra "do not call these from `OnSessionCreated`" restrictions. The pointer is the created session's control; the methods below are the documented members. HRESULT still has to be checked (`E_POINTER`, `AUDCLNT_E_DEVICE_INVALIDATED`, and for PID the success code `AUDCLNT_S_NO_SINGLE_PROCESS`).

| Field | On `IAudioSessionControl` / `IAudioSessionControl2`? | How, if at all |
| --- | --- | --- |
| PID | Yes, on `IAudioSessionControl2` | [`GetProcessId`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getprocessid). Writes a `DWORD`. If the session spans processes, returns `AUDCLNT_S_NO_SINGLE_PROCESS` and still writes the creating process's initial PID. |
| Device id / friendly name | No | Not a method on these interfaces. The device is the `IMMDevice` used to activate `IAudioSessionManager2`. |
| Data flow (capture / render) | No | Same: flow of that endpoint. A session is capture-only or render-only ([Audio Sessions](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-sessions)). |
| State | Yes, on `IAudioSessionControl` | [`GetState`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-getstate) → `AudioSessionStateInactive` / `Active` / `Expired` (`AudioSessionTypes.h`: 0 / 1 / 2). At first-stream `Initialize`, `GetState` and `AudioSessionState` say Inactive until a stream is running. |
| Volume | No | Not on `IAudioSessionControl` or `IAudioSessionControl2`. Session master volume is [`ISimpleAudioVolume::GetMasterVolume`](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-isimpleaudiovolume). Documented ways to get that interface: `IAudioClient::GetService(IID_ISimpleAudioVolume)` on a stream the caller owns, or [`IAudioSessionManager::GetSimpleAudioVolume`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager-getsimpleaudiovolume) with a **session GUID**. `OnSessionCreated` does not pass a session GUID. `GetSessionIdentifier` is a string, not that GUID. |
| Mute | No | Same: [`ISimpleAudioVolume::GetMute`](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-isimpleaudiovolume-getmute). |
| Session instance id | Yes, on `IAudioSessionControl2` | [`GetSessionInstanceIdentifier`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getsessioninstanceidentifier). Unique across instances; two playing instances of one app differ. Distinct from [`GetSessionIdentifier`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getsessionidentifier), which is shared by instances. Caller `CoTaskMemFree`s the string. |
| Display name | Yes, on `IAudioSessionControl` | [`GetDisplayName`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-getdisplayname). Empty if the client never called `SetDisplayName`; Sndvol then uses a generated default. |

`IAudioSessionControl` also has grouping param, icon path, and `RegisterAudioSessionNotification` (that last is the **other** notification interface; see §6).

Microsoft does not document `QueryInterface(ISimpleAudioVolume)` from the `OnSessionCreated` pointer. `ISimpleAudioVolume` is a separate `IUnknown` in `audioclient.h`. Whether that QI succeeds on a given Windows build is outside this note.

Default session volume at "initial activation" is 1.0 ([`ISimpleAudioVolume`](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-isimpleaudiovolume)). That is the session-manager default, not a value `OnSessionCreated` returns.

## 5. Already-running sessions: enumerate is still required

`RegisterSessionNotification` is **create**. It does not say it replays sessions that already exist.

Important note on that same page:

> You must call `IAudioSessionEnumerator::GetCount` to begin receiving notifications. The session enumeration API discards new session notifications until the application has first retrieved the list of existing sessions. This is to prevent a race condition that can occur when a session notification arrives while the application using the session APIs is starting up. Calling **GetCount** triggers the enumeration API to begin sending session notifications.

[`IAudioSessionEnumerator`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator) is the documented recipe:

1. MTA on a non-UI thread (`CoInitializeEx(NULL, COINIT_MULTITHREADED)`). Without MTA, "the application does not receive session notifications from the session manager." UI threads should be apartment-threaded.
2. `IMMDevice::Activate` → `IAudioSessionManager2`.
3. Implement `IAudioSessionNotification`.
4. `RegisterSessionNotification`.
5. `GetSessionEnumerator` — "generates a list of current sessions available for the endpoint."
6. Walk `GetSession` / `GetCount`. `AddRef` each control kept.
7. On `OnSessionCreated`, add that `IAudioSessionControl` to the app's list.

[`GetSessionEnumerator`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-getsessionenumerator): the manager "maintains a collection of audio sessions that are active on the audio device by querying the audio engine" and "creates a session control for each session in the collection."

[`GetCount`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionenumerator-getcount) "gets the total number of audio sessions that are open on the audio device."

The enumerator "might not be aware of the new sessions that are reported through `IAudioSessionNotification`." If the callback's `IAudioSessionControl` is released before the enumerator is initialized, the enumerator list can be partial. "If an application wants a complete set of sessions for the audio endpoint, the application should maintain its own list."

So: **already-open sessions are not delivered by `OnSessionCreated`.** **An enumerate pass is still required**, and **`GetCount` is also the gate that starts create callbacks.** Relying on the enumerator alone after that is documented as incomplete; new creates are meant to be merged from `OnSessionCreated`.

There is "no expiration mechanism enforced by the audio system on the session control objects" the application holds. A control stays valid while the app holds a reference.

## 6. Destroy / expire / Inactive are not this interface

`IAudioSessionNotification` has only `OnSessionCreated` (Learn methods table and `audiopolicy.h`). Unregister is "when a session is created."

State, volume, mute, and disconnect live on **`IAudioSessionEvents`**, registered **per session** with [`IAudioSessionControl::RegisterAudioSessionNotification`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-registeraudiosessionnotification) — a different method than `IAudioSessionManager2::RegisterSessionNotification`.

[`IAudioSessionEvents`](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionevents) methods include:

- `OnStateChanged(AudioSessionState)` — Active / Inactive / Expired
- `OnSessionDisconnected(AudioSessionDisconnectReason)`
- `OnSimpleVolumeChanged(float, BOOL, LPCGUID)` — volume or mute **change** after `SetMasterVolume` / `SetMute`

`RegisterAudioSessionNotification` remarks list: display name, volume, "session state changes (inactive to active, or active to inactive)," grouping, disconnection. Expired is on `OnStateChanged` even though that bullet names only inactive/active.

`AudioSessionState` / `GetState` / Audio Sessions: Inactive = streams exist but none running; Active = at least one running; Expired = no streams. Sndvol shows Active and Inactive rendering sessions and drops a row when the session expires or terminates (last stream released), except system-notification sounds.

So: **create of a new mixer row can come from `OnSessionCreated`. Going Inactive, Expired, or disconnected cannot.** Those require holding the `IAudioSessionControl` from create (or from enumerate) and registering `IAudioSessionEvents` on it. `GetState` is a poll of current state, not a destroy callback.

`IAudioSessionNotification` remarks: "The application must not register or unregister notification callbacks during an event callback." `IAudioSessionEvents` remarks: callbacks must be nonblocking; do not `UnregisterAudioSessionNotification` or release the last WASAPI reference during an event callback.

The UnregisterSessionNotification remarks sentence that mentions `IAudioSessionControl::RegisterAudioSessionNotification` names the wrong register method. The parameters, AddRef/Release behaviour, and "when a session is created" description still pair it with `RegisterSessionNotification`.

## 7. Apartment, thread, and lifetime (needed to receive the callback)

From RegisterSessionNotification and `IAudioSessionEnumerator`:

- MTA must be initialized on a non-UI thread or **no session notifications arrive**.
- UI threads should use the apartment threading model.
- `OnSessionCreated` runs on the **session manager thread**.
- Keep `NewSession` only after `AddRef`.
- Do not register/unregister from inside the callback.

`IAudioSessionControl2` also requires the using thread to be initialized for COM.

Minimum client for the session-notification / enumerator / `IAudioSessionControl2` surface: Windows 7 desktop (`audiopolicy.h` pages). `IAudioSessionControl` / `IAudioSessionEvents` / `AudioSessionState`: Windows Vista.

## What this note does not decide

- Which API Pipeline should use for Live session watch, or whether to implement a watcher.
- Whether volume, mute, and state on an existing row should live-update (`IAudioSessionEvents`) versus create-only (`IAudioSessionNotification`).
- How the session-manager-thread callback reaches a GUI STA thread.
- Whether `QueryInterface(ISimpleAudioVolume)` from the `OnSessionCreated` pointer is a supported contract (not in the Win32 pages).
- Exact interleaving of `OnSessionCreated` with `Initialize` returning, or whether `GetState` at callback time is always Inactive.
- New endpoints appearing after watch start (`IMMNotificationClient` is a different API).
- Whether exclusive-mode streams ever appear as sessions on this surface (`IAudioSessionControl` "does not work with exclusive-mode streams").
- Ducking (`RegisterDuckNotification` / `IAudioVolumeDuckNotification`) — a different `IAudioSessionManager2` pair, out of this ticket.

The 2026-08-21 design's "register `IAudioSessionNotification` on each active capture and render endpoint, and enumerate sessions that already exist" is consistent with the pages above as a **description of this API**, not as a product decision locked here.
