# Duck notification vs Live session create

Date: 2026-09-29. Ticket: [peilinok/winaudio#106](https://github.com/peilinok/winaudio/issues/106). Parent map: [peilinok/winaudio#105](https://github.com/peilinok/winaudio/issues/105).

Question: does `IAudioSessionManager2::RegisterDuckNotification` fire when a process opens or closes a Live session the Pipeline Inspector would list? A Live session is a Windows mixer row (PID + endpoint + flow), not a Track.

Pages and the SDK header were read on 2026-09-29. Claims about API behaviour are taken only from Microsoft Learn Win32 pages for this family and from `audiopolicy.h` in the Windows SDK. This note does not run the APIs.

## Short answer

**No.** `RegisterDuckNotification` does not report Live session create or destroy. After a successful register, the system calls `IAudioVolumeDuckNotification::OnVolumeDuckNotification` and `OnVolumeUnduckNotification` when a *communication stream* is opened or closed on the *default communication device*. Those callbacks name a session instance identifier string (and, on duck, a count of active communications sessions). They do not name process id, device, or data flow, and they do not supply the fields of a `LiveSessionView` row.

## What events fire

`IAudioVolumeDuckNotification` is the application-implemented sink passed to `RegisterDuckNotification`. The Learn page and the SDK header give it two methods besides `IUnknown`. There is no third duck method.

| Method | Parameters | What the docs say it is |
| --- | --- | --- |
| `OnVolumeDuckNotification` | `LPCWSTR sessionID`, `UINT32 countCommunicationSessions` | Notification that a system ducking event is pending / that ducking begins |
| `OnVolumeUnduckNotification` | `LPCWSTR sessionID` | Notification that a system unducking event is pending / that ducking ends |

Sources: [IAudioVolumeDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiovolumeducknotification), [OnVolumeDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiovolumeducknotification-onvolumeducknotification), [OnVolumeUnduckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiovolumeducknotification-onvolumeunducknotification). The same two methods appear in `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\audiopolicy.h` (`IAudioVolumeDuckNotification`, IID `C3B284D4-6D39-4359-B3CF-B56DDB3BB39C`).

[Implementation Considerations for Ducking Notifications](https://learn.microsoft.com/en-us/windows/win32/coreaudio/handling-audio-ducking-events-from-communication-devices): "The session manager handling the communication session calls `IAudioVolumeDuckNotification::OnVolumeDuckNotification` when the communication stream opens and then calls `IAudioVolumeDuckNotification::OnVolumeUnduckNotification` when the stream is closed on the communication device."

The `sessionID` argument on both callbacks is "the session instance identifier of the communications session" that raises ducking (duck) or of "the terminating communications session that initiated the ducking" (unduck). The docs point at `IAudioSessionControl2::GetSessionInstanceIdentifier` as the way to obtain that string from a session the caller already holds. `countCommunicationSessions` is "the number of active communications sessions."

`RegisterDuckNotification` itself is the registration call. It returns `S_OK`, `E_POINTER` (`duckNotification` is NULL), or `E_OUTOFMEMORY`. It is not an event. Unregister is `UnregisterDuckNotification`; after that call, pending events are not reported.

## Preconditions for those events

Minimum OS on every duck page and on the header: Windows 7, Windows Server 2008 R2; desktop apps only.

### What the system raises the events for

[IAudioSessionManager2::RegisterDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registerducknotification) remarks: "For stream attenuation, a session event is raised by the system when a communication stream is opened or closed on the default communication device." The same sentence is on [IAudioSessionManager2](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionmanager2) and [IAudioSessionControl2](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessioncontrol2).

[Using a Communication Device](https://learn.microsoft.com/en-us/windows/win32/coreaudio/using-the-communication-device) defines the terms:

- A *communication device* is the endpoint the user sets as **Default Communication Device** (`eCommunications` role) on the Playback and Recording tabs. `IMMDeviceEnumerator::GetDefaultAudioEndpoint` with `Role` = `eCommunications` is how a WASAPI client gets that endpoint.
- "The operating system considers the stream opened on a communication device to be a *communication stream*."
- "The audio system generates ducking events when a communication stream is opened or closed for rendering or capturing streams."

So the documented trigger is open/close of a stream that the OS treats as a communication stream, on the default communication device, capture or render. It is not "any new mixer row."

[Default Ducking Experience](https://learn.microsoft.com/en-us/windows/win32/coreaudio/stream-attenuation) adds: new *non-communication* streams may open during a communication session and "are not automatically attenuated." When all communication streams are closed, the system ends the communication session and restores volume. That is the unduck side. Opening a non-communication stream is not described as a ducking event.

The SDK sample that *causes* ducking events, [DuckingCaptureSample](https://learn.microsoft.com/en-us/windows/win32/coreaudio/duckingcapturesample), "demonstrates opening and closing communication streams and causing ducking events." It "uses MMDevice API to get a reference to the default rendering or capture communication device," then starts a chat session on that device.

### `sessionID` vs `NULL` at register time

`RegisterDuckNotification`'s first parameter is the *listener's* session, not a filter of which Live sessions to watch.

- "Applications that are playing a media stream and want to provide custom stream attenuation or ducking behavior, pass their own session instance identifier."
- "Other applications that do not want to alter their streams but want to get all the ducking notifications must pass **NULL**."

[Getting Ducking Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/getting-ducking-events-from-a-communication-device) walks the media app's own playback device (the sample uses `GetDefaultAudioEndpoint(eRender, eConsole)`), reads that session's instance identifier, and passes it to `RegisterDuckNotification`. The point of that string is so "the audio system [knows] which audio session is listening for the ducking events." After that, "the application can now receive event notification when a stream opened on the communication device."

Passing NULL vs a session instance identifier therefore chooses whether this registrant is a custom-ducking media session or a listener for all ducking notifications. It does not subscribe to ordinary session create/destroy on an endpoint.

[IAudioSessionControl2](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessioncontrol2) remarks say the same: the session identifier "is required during the notification registration. The application can register itself to receive ducking notifications from the system."

### Related: opt-out does not stop the callbacks

[Disabling the Default Ducking Experience](https://learn.microsoft.com/en-us/windows/win32/coreaudio/disabling-the-ducking-experience): a WASAPI client can call `IAudioSessionControl2::SetDuckingPreference` to opt out of the *default attenuation*. "Note that even if an client opts out of ducking, it still receives ducking notifications from the system." A user can also turn the default experience off on the Communications tab of Mmsys.cpl; that page does not say whether that user setting suppresses the callbacks.

### Stream category (Windows 8+)

[AUDIO_STREAM_CATEGORY](https://learn.microsoft.com/en-us/windows/win32/api/audiosessiontypes/ne-audiosessiontypes-audio_stream_category) (`audiosessiontypes.h`, Windows 8) includes `AudioCategory_Communications` ("Real-time communications, such as VOIP or chat") and notes that `AudioCategory_GameChat` is similar "except that **AudioCategory_GameChat** will not attenuate other streams." [Detect audio format capabilities for communications scenarios](https://learn.microsoft.com/en-us/windows/win32/coreaudio/communications-audio-format-capabilities) says applications using devices for communications should categorize streams as `AudioCategory_Communications`, and that this "engages the right user experience behavior in the OS (such as ducking)."

Those pages do not name `RegisterDuckNotification` or `IAudioVolumeDuckNotification`. This note therefore does not treat stream category as a documented substitute for "stream opened on a communication device" in the duck-callback contract. It only records that later Core Audio docs still tie ducking to communications streams, not to every capture or render session.

## Ordinary Live session create / destroy

A Live session, for this map, is a mixer row: one Windows audio session on one endpoint for one process (PID + endpoint + flow).

The duck pages never describe session enumeration, mixer rows, or "a process started capture or playback" as the event. They describe communication-stream open/close on the default communication device.

Consequences that follow from those pages, without extra experiments:

- Starting or stopping capture or playback on an ordinary render or capture endpoint (the default console/multimedia device, or any endpoint that is not the default communication device) is not the documented trigger. [Using a Communication Device](https://learn.microsoft.com/en-us/windows/win32/coreaudio/using-the-communication-device) distinguishes the `eCommunications` role from the multimedia (`eConsole`) role for that reason.
- A new non-communication stream during an existing call is explicitly *not* auto-attenuated ([Default Ducking Experience](https://learn.microsoft.com/en-us/windows/win32/coreaudio/stream-attenuation)). That is the opposite of "every new Live session raises a duck notification."
- The one overlap: if a process opens a stream *on the default communication device*, the OS "considers the stream opened on a communication device to be a communication stream," and ducking events are generated for that open/close. That process would also appear as a mixer row. The notification is still "ducking began / ended because a communications stream opened / closed," not "here is a new Live session." It would not fire for the rest of the Inspector radar (ordinary capture and render sessions on other endpoints).

The same `IAudioSessionManager2` interface documents a different method, `RegisterSessionNotification`, as registering "to receive a notification when a session is created." That sentence is evidence that session-created is not what `RegisterDuckNotification` is for. This note does not evaluate `RegisterSessionNotification`.

## Can the callback fill a `LiveSessionView`?

`LiveSessionView` (as the Inspector list row) carries `processId`, `processName`, `deviceId`, `deviceName`, `flow` (capture or render), `sessionVolume`, `sessionMute`, and `state`.

Duck callbacks supply:

- `sessionID`: a session *instance* identifier string for the communications session that caused duck/unduck.
- `countCommunicationSessions` (duck only): a count.

They do not supply PID, process name, device id, device name, capture vs render, volume, mute, or session state.

[GetSessionInstanceIdentifier](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getsessioninstanceidentifier) describes that string as unique per session instance and different from the shared session identifier. It does not document the string as encoding PID, device, or flow, and it does not define a parser. PID is a separate call, [IAudioSessionControl2::GetProcessId](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol2-getprocessid), on an `IAudioSessionControl2` the caller already has. The duck callback does not pass an `IAudioSessionControl` (or `IAudioSessionControl2`) pointer.

So the notification does not name the new session's PID, device, or data flow in a way that could fill a `LiveSessionView` row. Even a later lookup from the instance-id string would be extra WASAPI work this interface does not describe, and it would still only be about the communications session that caused ducking.

## Header check

`audiopolicy.h` in Windows SDK 10.0.26100.0 matches the Learn signatures:

- `IAudioSessionManager2::RegisterDuckNotification(LPCWSTR sessionID, IAudioVolumeDuckNotification *duckNotification)`
- `IAudioVolumeDuckNotification::OnVolumeDuckNotification(LPCWSTR sessionID, UINT32 countCommunicationSessions)`
- `IAudioVolumeDuckNotification::OnVolumeUnduckNotification(LPCWSTR sessionID)`

No other duck callback is declared. The header has no remarks that widen the event to arbitrary session create/destroy.

## What this note does not decide

- Which API, if any, Pipeline should use to watch Live sessions. That is a later ticket. This note only answers whether `RegisterDuckNotification` is a Live-session create/destroy signal.
- Whether to implement a watcher, how to marshal callbacks, or whether session instance id belongs on `LiveSessionView`.
- Whether a stream categorized `AudioCategory_Communications` on a non-communications endpoint raises these callbacks. The duck-callback pages do not say.
- Whether the Communications-tab user setting that disables default attenuation also suppresses the callbacks.
- Runtime confirmation on a machine with real devices. The contract above is what the Win32 pages and `audiopolicy.h` state.
