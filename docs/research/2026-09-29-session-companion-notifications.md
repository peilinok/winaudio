# Companion notifications for Live session end, state, and new endpoints

Date: 2026-09-29. Ticket: [peilinok/winaudio#108](https://github.com/peilinok/winaudio/issues/108). Parent map: [peilinok/winaudio#105](https://github.com/peilinok/winaudio/issues/105).

This note does not implement a watcher and does not pick which subscriptions Pipeline should take. It records what the public Core Audio notification interfaces actually report, from Microsoft Learn Win32 docs and the Windows SDK headers they name (`audiopolicy.h`, `mmdeviceapi.h`, `audiosessiontypes.h`, `endpointvolume.h`; local copies from Windows Kits 10.0.26100.0).

A Live session is a mixer row (`LiveSessionView`: process, device, flow, session volume, session mute, `AudioSessionState`), not a Track.

Assume a watcher that already enumerated existing sessions on the capture and render endpoints that were present at start. The remaining question is what else must be subscribed so that list can stay current without Refresh.

## Short answer

`IAudioSessionNotification::OnSessionCreated` reports only that a session was created on the endpoint whose `IAudioSessionManager2` you registered. It does not report session end, Active/Inactive/Expired, volume, mute, or a device that appears later.

To cover those facts from public Core Audio:

- **New sessions on an endpoint you already hold:** `IAudioSessionManager2::RegisterSessionNotification` → `IAudioSessionNotification::OnSessionCreated`, after `IAudioSessionEnumerator::GetCount`. Register on that endpoint's `IAudioSessionManager2`.
- **End, state, volume, mute on a session you already hold:** `IAudioSessionControl::RegisterAudioSessionNotification` → `IAudioSessionEvents`. Register on that session's `IAudioSessionControl`. `OnStateChanged(AudioSessionStateExpired)` and `OnSessionDisconnected` are the public session-end signals; there is no `OnSessionDestroyed`.
- **New or departing endpoints:** `IMMDeviceEnumerator::RegisterEndpointNotificationCallback` → `IMMNotificationClient`. Register once on the enumerator (system-wide). Session-create is still per-manager, so a newly active endpoint still needs its own `IAudioSessionManager2` plus `RegisterSessionNotification` and an enumerate.

Duck notifications and endpoint-volume callbacks do not add session-end, session-state, or new-endpoint facts. Mixer-row volume/mute is `OnSimpleVolumeChanged`.

## Coverage map

| Fact on a mixer row | Public notification | Register on |
| --- | --- | --- |
| Session already present at start | None. `IAudioSessionManager2::GetSessionEnumerator` + `GetCount` / `GetSession` ([IAudioSessionEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator)). | `IAudioSessionManager2` activated from that `IMMDevice`. |
| New session on a known endpoint | `IAudioSessionNotification::OnSessionCreated` | `IAudioSessionManager2::RegisterSessionNotification` on that endpoint. |
| Active / Inactive / Expired | `IAudioSessionEvents::OnStateChanged` | `IAudioSessionControl::RegisterAudioSessionNotification` on that session. |
| Session end (no streams; Sndvol drops the slider) | `OnStateChanged(AudioSessionStateExpired)` | Same `IAudioSessionEvents` registration. |
| Session end (device gone, service down, format change, logoff, exclusive override) | `IAudioSessionEvents::OnSessionDisconnected` | Same. |
| Session master volume or mute | `IAudioSessionEvents::OnSimpleVolumeChanged` | Same. |
| Endpoint added, removed, or state change (active / disabled / not present / unplugged) | `IMMNotificationClient` `OnDeviceAdded` / `OnDeviceRemoved` / `OnDeviceStateChanged` | `IMMDeviceEnumerator::RegisterEndpointNotificationCallback` (one client, all endpoints). |
| New sessions on an endpoint that was not present at start | `OnDeviceAdded` or `OnDeviceStateChanged(DEVICE_STATE_ACTIVE)`, then a new `IAudioSessionManager2` + `RegisterSessionNotification` + enumerate | Enumerator, then that device's manager. |

`IAudioSessionNotification` has only `OnSessionCreated` (`audiopolicy.h`). End is not a session-manager callback.

## `IAudioSessionEvents` — per session

**Register on:** the session's `IAudioSessionControl` (or `IAudioSessionControl2`, which inherits the same method), via `IAudioSessionControl::RegisterAudioSessionNotification`. The client implements `IAudioSessionEvents`. Vista+. Header: `audiopolicy.h`.

The session manager then calls the interface for display-name changes, volume changes, session state changes (inactive ↔ active), grouping-parameter changes, and disconnection (endpoint removed, session manager shut down, or stream format changed). ([RegisterAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-registeraudiosessionnotification); [IAudioSessionEvents](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionevents); [Audio Session Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-session-events).)

SDK 10.0.26100.0 `audiopolicy.h` lists seven methods besides `IUnknown`:

| Method | What it notifies | Mixer-row fact |
| --- | --- | --- |
| `OnStateChanged(AudioSessionState NewState)` | Stream-activity state. `AudioSessionStateActive`, `AudioSessionStateInactive`, `AudioSessionStateExpired` ([OnStateChanged](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionevents-onstatechanged); [AudioSessionState](https://learn.microsoft.com/en-us/windows/win32/api/audiosessiontypes/ne-audiosessiontypes-audiosessionstate)). | **State**, and **end** when `Expired`. |
| `OnSessionDisconnected(AudioSessionDisconnectReason)` | Session disconnected. Reasons in `audiopolicy.h`: `DisconnectReasonDeviceRemoval`, `DisconnectReasonServerShutdown`, `DisconnectReasonFormatChanged`, `DisconnectReasonSessionLogoff`, `DisconnectReasonSessionDisconnected`, `DisconnectReasonExclusiveModeOverride` ([OnSessionDisconnected](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionevents-onsessiondisconnected)). | **End** (streams closed / `IAudioClient` invalidated). |
| `OnSimpleVolumeChanged(float NewVolume, BOOL NewMute, LPCGUID EventContext)` | Session master volume (0.0–1.0) or mute after `ISimpleAudioVolume::SetMasterVolume` / `SetMute` ([OnSimpleVolumeChanged](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionevents-onsimplevolumechanged)). | **Volume and mute** on `LiveSessionView`. |
| `OnChannelVolumeChanged(...)` | Per-channel session submix via `IChannelAudioVolume` ([OnChannelVolumeChanged](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionevents-onchannelvolumechanged)). | Not the master volume field. |
| `OnDisplayNameChanged` | `IAudioSessionControl::SetDisplayName` ([OnDisplayNameChanged](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionevents-ondisplaynamechanged)). | Not a `LiveSessionView` field (row uses process name). |
| `OnIconPathChanged` | `SetIconPath` ([OnIconPathChanged](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionevents-oniconpathchanged)). | Not a `LiveSessionView` field. |
| `OnGroupingParamChanged` | `SetGroupingParam` ([OnGroupingParamChanged](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionevents-ongroupingparamchanged)). | Not a `LiveSessionView` field. |

### State versus end

`AudioSessionState` in `audiosessiontypes.h`:

- `AudioSessionStateInactive` (0): at least one stream, none running.
- `AudioSessionStateActive` (1): at least one stream running.
- `AudioSessionStateExpired` (2): no streams. Header comment: "The session is dormant." Docs: "The audio session has expired. (It contains no streams.)"

Learn: a client `Initialize`s the first stream → inactive; `Start` → active; `Stop` of the last running stream → inactive; releasing the last stream object → expired. The system is always the source of `OnStateChanged` (no `EventContext`). ([OnStateChanged](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionevents-onstatechanged) remarks; [AudioSessionState](https://learn.microsoft.com/en-us/windows/win32/api/audiosessiontypes/ne-audiosessiontypes-audiosessionstate).)

[Audio Sessions](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-sessions): Sndvol shows volume controls for active and inactive rendering sessions; it removes them when the session goes inactive → expired, or when the session terminates (last stream deleted). Inactive is not end.

`OnSessionDisconnected` is a different end: the session manager closes the session's streams and invalidates outstanding `IAudioClient` / `GetService` objects. The client cannot raise this event. If the audio service dies without notice, clients instead see `AUDCLNT_E_SERVICE_NOT_RUNNING` on a later call. ([OnSessionDisconnected](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionevents-onsessiondisconnected).)

There is no public "session destroyed" callback on `IAudioSessionNotification` or `IAudioSessionManager2`.

### What `IAudioSessionEvents` does not notify

- A session created after the snapshot (`OnSessionCreated` is a different interface).
- An endpoint that is plugged in, unplugged, enabled, or disabled.
- Default-device role changes.
- Communication ducking (pending duck/unduck of other streams).
- Endpoint master volume (`IAudioEndpointVolume`).

It only fires for the session whose `IAudioSessionControl` you registered on. New sessions from `OnSessionCreated` need their own `RegisterAudioSessionNotification` if those rows should keep updating.

### Lifetime and callback rules

`RegisterAudioSessionNotification` `AddRef`s the client's `IAudioSessionEvents`. Unregister before the last client release, or the session manager leaks the pair. Notifications continue only while the client still holds `IAudioSessionControl` or `IAudioSessionManager`. Do not unregister or drop the last WASAPI ref from inside a callback; callbacks must not block. ([RegisterAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-registeraudiosessionnotification) remarks; [IAudioSessionEvents](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionevents).)

`IAudioSessionControl` is for shared-mode sessions, not exclusive-mode streams. ([IAudioSessionControl](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessioncontrol).)

## `IAudioSessionNotification` — per endpoint manager (new sessions)

**Register on:** `IAudioSessionManager2` of one audio endpoint (`IMMDevice::Activate` with `IID_IAudioSessionManager2`), via `RegisterSessionNotification`. The application implements `IAudioSessionNotification`. Windows 7+. Header: `audiopolicy.h`.

The interface has one method: `OnSessionCreated(IAudioSessionControl *NewSession)`. The audio engine calls it when a new session is activated on that device endpoint, from the session manager thread. Keep a reference to `NewSession` if it is needed after the callback returns. ([IAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionnotification); [OnSessionCreated](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionnotification-onsessioncreated); [RegisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registersessionnotification).)

### What it does not notify

- Session end, state, volume, mute, display name, icon, grouping, or disconnect. No other methods exist on the interface (`audiopolicy.h`).
- Sessions on a different endpoint. Each manager is activated from one `IMMDevice` ([IAudioSessionManager2](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionmanager2)).
- Endpoint arrival. A USB device plugged in after registration is a new `IMMDevice` and needs a new manager.

### Enumerator lag and GetCount

The session enumerator can miss sessions that arrived through `OnSessionCreated`. If the `IAudioSessionControl` from the callback is released before the enumerator is initialized, a later `GetSessionEnumerator` walk can be incomplete. Docs tell the application to keep its own list and `AddRef` each session control, including the pointer from `OnSessionCreated`. The audio system does not expire those controls while the application holds them. ([IAudioSessionEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator) remarks; [IAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionnotification).)

`RegisterSessionNotification` remarks: **you must call `IAudioSessionEnumerator::GetCount` to begin receiving notifications.** Until the application has retrieved the existing-session list, new-session notifications are discarded (startup race). `GetCount` starts delivery. ([RegisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registersessionnotification).)

A watcher that already enumerated current endpoints has already called `GetCount` on those managers. That does not start delivery on a manager created later for a newly plugged device.

COM: session notifications require MTA (`CoInitializeEx(NULL, COINIT_MULTITHREADED)` on a non-UI thread). Without MTA, the application does not receive them. UI threads stay STA. Do not register or unregister during a callback. ([IAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionnotification); [IAudioSessionEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator) steps 1–7.)

## `IMMNotificationClient` — system-wide endpoints

**Register on:** `IMMDeviceEnumerator` (the `MMDeviceEnumerator` COM object), via `RegisterEndpointNotificationCallback`. The client implements `IMMNotificationClient`. Vista+. Header: `mmdeviceapi.h`.

The system calls this when endpoint **roles, state, existence, or properties** change. One registration receives **all event types on all audio endpoints**. Filter in the callbacks if only some devices matter. ([RegisterEndpointNotificationCallback](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-registerendpointnotificationcallback); [IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient); [Device Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-events).)

SDK 10.0.26100.0 `mmdeviceapi.h` lists five methods besides `IUnknown`:

| Method | What it notifies | Mixer-row fact |
| --- | --- | --- |
| `OnDeviceAdded(LPCWSTR pwstrDeviceId)` | A new audio endpoint was added ([OnDeviceAdded](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immnotificationclient-ondeviceadded)). | **New endpoint.** Then activate `IAudioSessionManager2` on that id and enumerate / `RegisterSessionNotification`. |
| `OnDeviceRemoved(LPCWSTR pwstrDeviceId)` | An endpoint was removed ([OnDeviceRemoved](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immnotificationclient-ondeviceremoved)). | Rows on that `deviceId` go away. Existing sessions may also `OnSessionDisconnected(DisconnectReasonDeviceRemoval)`. |
| `OnDeviceStateChanged(LPCWSTR pwstrDeviceId, DWORD dwNewState)` | Endpoint state: `DEVICE_STATE_ACTIVE` (0x1), `DISABLED` (0x2), `NOTPRESENT` (0x4), `UNPLUGGED` (0x8) ([OnDeviceStateChanged](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immnotificationclient-ondevicestatechanged); [DEVICE_STATE_XXX](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-state-xxx-constants)). | An endpoint that becomes `ACTIVE` is newly visible to a walk that uses `EnumAudioEndpoints(..., DEVICE_STATE_ACTIVE)` (WinAudio's `DeviceEnumerator` does). `DISABLED` / `NOTPRESENT` / `UNPLUGGED` drop it from that walk. Jack-presence can move `UNPLUGGED` ↔ `ACTIVE` without a new id. |
| `OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR pwstrDefaultDeviceId)` | Default device for a role (`eConsole` / `eMultimedia` / `eCommunications`) changed. `pwstrDefaultDeviceId` may be `NULL` if no device can take the role ([OnDefaultDeviceChanged](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immnotificationclient-ondefaultdevicechanged)). | **Not** a new endpoint by itself, if the list is all active capture/render devices rather than defaults only. Apps that reopen on the new default still show up as `OnSessionCreated` on that manager (and often disconnect on the old one). |
| `OnPropertyValueChanged(LPCWSTR pwstrDeviceId, const PROPERTYKEY key)` | A property in the endpoint property store changed ([OnPropertyValueChanged](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immnotificationclient-onpropertyvaluechanged)). | Not session end/state/volume. Can change `deviceName` if `PKEY_Device_FriendlyName` changes. |

Device-event examples: enable/disable in Mmsys.cpl or Device Manager; add/remove an adapter (events for every endpoint on that adapter); jack plug/unplug with jack-presence detection; default role change; property change. ([Device Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-events).)

Treat the endpoint ID string as opaque; use `IMMDeviceEnumerator::GetDevice` or compare to `IMMDevice::GetId`. ([IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient).)

### What `IMMNotificationClient` does not notify

- Session create, expire, disconnect, Active/Inactive, or session volume/mute.
- Anything about a mixer row except through device identity (and maybe friendly name).

### Lifetime and callback rules

Current Learn: `RegisterEndpointNotificationCallback` / `UnregisterEndpointNotificationCallback` **do not** `AddRef`/`Release` the client's `IMMNotificationClient`. The client must keep the object alive until after unregister. Callbacks must not block, must not register/unregister the enumerator from inside the callback, and must not drop the last MMDevice ref during the callback. ([RegisterEndpointNotificationCallback](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-registerendpointnotificationcallback); [IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient).)

## Other Core Audio notifications (do not add the asked facts)

Public headers under Windows Kits 10.0.26100.0 name these further notification sinks next to session/endpoint APIs. They do not report Live session end, `AudioSessionState`, or new endpoints.

### `IAudioVolumeDuckNotification`

**Register on:** `IAudioSessionManager2::RegisterDuckNotification(sessionID, duckNotification)`. Pass a session instance id to customize ducking for that session, or `NULL` to observe all ducking events. Windows 7+. ([RegisterDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registerducknotification); [IAudioVolumeDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiovolumeducknotification).)

Methods in `audiopolicy.h`: `OnVolumeDuckNotification(LPCWSTR sessionID, UINT32 countCommunicationSessions)` and `OnVolumeUnduckNotification(LPCWSTR sessionID)`. These fire when a communication stream opens or closes on the default communication device (stream attenuation / ducking).

They do not report mixer-row create, destroy, Active/Inactive/Expired, or endpoint arrival. If default ducking changes a session's master volume or mute, that still arrives as `OnSimpleVolumeChanged` on that session's `IAudioSessionEvents`.

### `IAudioEndpointVolumeCallback`

**Register on:** `IAudioEndpointVolume::RegisterControlChangeNotify` for one endpoint (`endpointvolume.h`). `OnNotify` reports that endpoint's master/channel volume or mute, not `ISimpleAudioVolume` on a session. ([IAudioEndpointVolumeCallback](https://learn.microsoft.com/en-us/windows/win32/api/endpointvolume/nn-endpointvolume-iaudioendpointvolumecallback); [Device Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-events) points at this for "endpoint-volume events".)

`LiveSessionView.sessionVolume` / `sessionMute` come from `ISimpleAudioVolume` on the session, which is `OnSimpleVolumeChanged`.

### `IAudioEffectsChangedNotificationClient`

**Register on:** `IAudioEffectsManager::RegisterAudioEffectsChangedNotificationCallback` (`Audioclient.h`). Stream-effect list changes after attach, not mixer-row membership or session state.

`IAudioSessionControl2` adds `GetProcessId`, `GetSessionIdentifier`, `GetSessionInstanceIdentifier`, `IsSystemSoundsSession`, and `SetDuckingPreference`. It does not add notification methods beyond the inherited `RegisterAudioSessionNotification`. ([IAudioSessionControl2](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessioncontrol2).)

## What must be subscribed for a list that stays current

Factual composition only. Which of these Pipeline takes is a later ticket (including whether volume/mute/state live-update or only create/destroy).

Given an initial enumerate of sessions on current capture and render endpoints:

1. **Keep each of those endpoints' `IAudioSessionManager2` alive** and `RegisterSessionNotification` on it (with `GetCount` already called during enumerate) so later sessions on those devices raise `OnSessionCreated`. Dropping the manager after the walk, as today's `LiveSessionEnumerator` does, means no further create callbacks.
2. **On every session control you keep** (enumerator + `OnSessionCreated`): `RegisterAudioSessionNotification` so `OnStateChanged`, `OnSessionDisconnected`, and `OnSimpleVolumeChanged` can update or remove the row. Create alone never expires or disconnects a row.
3. **Once on `IMMDeviceEnumerator`:** `RegisterEndpointNotificationCallback`. On `OnDeviceAdded` / `OnDeviceStateChanged(DEVICE_STATE_ACTIVE)`, activate a new manager, enumerate existing sessions on that device, and `RegisterSessionNotification`. On remove / non-active state, drop rows for that `deviceId` (and the manager registration). `OnSessionCreated` registered only on the original device set cannot see a device that was plugged in afterwards.

Refresh remains the docs' recovery path when a notification is missed; this note does not decide whether Pipeline keeps a Refresh button.

## Sources

Microsoft Learn (fetched 2026-09-29):

- [IAudioSessionEvents](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionevents) and methods `OnStateChanged`, `OnSessionDisconnected`, `OnSimpleVolumeChanged`, `OnChannelVolumeChanged`, `OnDisplayNameChanged`, `OnIconPathChanged`, `OnGroupingParamChanged`
- [IAudioSessionControl::RegisterAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-registeraudiosessionnotification), [IAudioSessionControl](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessioncontrol), [IAudioSessionControl2](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessioncontrol2)
- [Audio Session Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-session-events), [Audio Sessions](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-sessions), [AudioSessionState](https://learn.microsoft.com/en-us/windows/win32/api/audiosessiontypes/ne-audiosessiontypes-audiosessionstate)
- [IAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionnotification), [OnSessionCreated](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionnotification-onsessioncreated), [IAudioSessionManager2::RegisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registersessionnotification), [IAudioSessionManager2](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionmanager2), [IAudioSessionEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator)
- [IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient) and methods `OnDeviceAdded`, `OnDeviceRemoved`, `OnDeviceStateChanged`, `OnDefaultDeviceChanged`, `OnPropertyValueChanged`
- [IMMDeviceEnumerator::RegisterEndpointNotificationCallback](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-registerendpointnotificationcallback), [IMMDeviceEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immdeviceenumerator), [Device Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-events), [DEVICE_STATE_XXX](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-state-xxx-constants)
- [IAudioVolumeDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiovolumeducknotification), [RegisterDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registerducknotification)
- [IAudioEndpointVolumeCallback](https://learn.microsoft.com/en-us/windows/win32/api/endpointvolume/nn-endpointvolume-iaudioendpointvolumecallback)

Windows SDK 10.0.26100.0 headers (local): `um\audiopolicy.h`, `um\mmdeviceapi.h`, `um\AudioSessionTypes.h`, `um\endpointvolume.h`, `um\Audioclient.h`.
