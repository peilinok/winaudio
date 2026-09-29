# Session notification threading

Date: 2026-09-29. Ticket: [peilinok/winaudio#109](https://github.com/peilinok/winaudio/issues/109). Parent map: [peilinok/winaudio#105](https://github.com/peilinok/winaudio/issues/105) (Pipeline Live session watch spec).

Question: on which thread do Core Audio session notifications run, and what must stay alive after register? Primary sources only: Microsoft Learn Win32 pages for the four register APIs and the COM apartment pages those remarks cite, plus Windows SDK 10.0.26100 `audiopolicy.h` / `mmdeviceapi.h`. This note does not implement a watcher and does not pick how `AppUi` wires a sink.

A Live session is a mixer row, not a Track (`CONTEXT.md`).

## Short answer

`IAudioSessionNotification::OnSessionCreated` is invoked on the **session manager thread**, not on the thread that called `RegisterSessionNotification` and not on this repo's capture/render pump. Session-create watch needs **MTA on a non-UI thread** (`CoInitializeEx(NULL, COINIT_MULTITHREADED)`); without that MTA the process gets no session-create callbacks. UI threads should be STA. The registering thread for that MTA does **not** pump messages. Ducking and session-event samples still `PostMessage` to an HWND and say not to block the notification thread.

Keep alive for as long as the watch should fire: one `IAudioSessionManager2` per watched endpoint, the callback sink, and (for per-session events) the `IAudioSessionControl` you registered on. Unregister from outside the callback. In-flight callbacks must not drop the last WASAPI/MMDevice ref and must not wait on a lock. Heap allocation is not forbidden. The ImGui `PeekMessage` loop in `src/gui/main.cpp` is a valid place to **receive** posted UI work; it is **not** the apartment Microsoft says to register session-create notifications on. The documented pattern is: register from MTA, marshal UI work (the samples use `PostMessage`).

## Sources

Microsoft Learn (fetched 2026-09-29):

- [IAudioSessionManager2::RegisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registersessionnotification)
- [IAudioSessionManager2::UnregisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-unregistersessionnotification)
- [IAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionnotification)
- [IAudioSessionNotification::OnSessionCreated](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionnotification-onsessioncreated)
- [IAudioSessionEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator)
- [IAudioSessionManager2](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionmanager2)
- [IAudioSessionManager2::RegisterDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registerducknotification)
- [IAudioSessionManager2::UnregisterDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-unregisterducknotification)
- [IAudioVolumeDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiovolumeducknotification)
- [Getting Ducking Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/getting-ducking-events-from-a-communication-device)
- [Implementation Considerations for Ducking Notifications](https://learn.microsoft.com/en-us/windows/win32/coreaudio/handling-audio-ducking-events-from-communication-devices)
- [IAudioSessionControl::RegisterAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-registeraudiosessionnotification)
- [IAudioSessionControl::UnregisterAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-unregisteraudiosessionnotification)
- [IAudioSessionEvents](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionevents)
- [Audio Session Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-session-events)
- [IMMDeviceEnumerator::RegisterEndpointNotificationCallback](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-registerendpointnotificationcallback)
- [IMMDeviceEnumerator::UnregisterEndpointNotificationCallback](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-unregisterendpointnotificationcallback)
- [IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient)
- [Device Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-events)
- [CoInitializeEx](https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex) (cited by `IAudioSessionManager2`)
- [Processes, Threads, and Apartments](https://learn.microsoft.com/en-us/windows/win32/com/processes--threads--and-apartments)
- [Single-Threaded Apartments](https://learn.microsoft.com/en-us/windows/win32/com/single-threaded-apartments)
- [Multithreaded Apartments](https://learn.microsoft.com/en-us/windows/win32/com/multithreaded-apartments)

SDK headers on this machine: `C:\Program Files (x86)\Windows Kits\10\Include\10.0.26100.0\um\audiopolicy.h` and `mmdeviceapi.h`. They declare the methods. They do not copy the Learn remarks about MTA, the session manager thread, or callback rules.

## Which thread / apartment

### Session create (`IAudioSessionNotification`)

[OnSessionCreated](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionnotification-onsessioncreated): "The audio engine calls **OnSessionCreated** when a new session is activated on the device endpoint. This method is called from the session manager thread."

That is a system thread owned by the session manager, not the caller's register thread.

[RegisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registersessionnotification) and [IAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionnotification) and [IAudioSessionEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator) all carry the same note:

> Make sure that the application initializes COM with Multithreaded Apartment (MTA) model by calling `CoInitializeEx(NULL, COINIT_MULTITHREADED)` in a non-UI thread. If MTA is not initialized, the application does not receive session notifications from the session manager. Threads that run the user interface of an application should be initialized apartment threading model.

[IAudioSessionEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator) lists that MTA step as step 1 of the recipe to receive create notifications and keep a session list.

MTA does not use a window message pump. [Multithreaded Apartments](https://learn.microsoft.com/en-us/windows/win32/com/multithreaded-apartments): "The threads need not retrieve and dispatch messages because COM does not use window messages in this model." So the **registering** MTA thread need not pump messages for these callbacks to arrive. They arrive on the session manager thread.

The Learn sample for `IAudioSessionNotification::OnSessionCreated` does not touch UI on that thread. It posts to the main HWND:

```cpp
HRESULT OnSessionCreated(IAudioSessionControl *pNewSession)
{
    if (pNewSession)
    {
        PostMessage(m_hwndMain, WM_SESSION_CREATED, 0, 0);
    }
}
```

([IAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionnotification) Examples.)

[RegisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registersessionnotification) also: you must call `IAudioSessionEnumerator::GetCount` after register or the API discards new-session notifications until the existing list has been retrieved. Calling `GetCount` is what starts delivery.

### Per-session events (`IAudioSessionEvents`)

[IAudioSessionEvents](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionevents): after `IAudioSessionControl::RegisterAudioSessionNotification`, "the client receives event notifications in the form of callbacks through the methods in the interface." [Audio Session Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-session-events): the methods "receive callbacks from the WASAPI system module."

Those pages do **not** name a thread the way `OnSessionCreated` names "the session manager thread." They do not repeat the MTA-on-non-UI-thread note. They do require the methods to be nonblocking (below).

### Ducking (`IAudioVolumeDuckNotification`)

[Implementation Considerations for Ducking Notifications](https://learn.microsoft.com/en-us/windows/win32/coreaudio/handling-audio-ducking-events-from-communication-devices): "The ducking notifications are received asynchronously in the background and the media application must not block the notification thread to process the window messages. The window messages must be processed on the user interface thread."

The same page and the [IAudioVolumeDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiovolumeducknotification) sample both `PostMessage` duck/unduck to an HWND and return. They do not name that notification thread as the session manager thread, only as a background notification thread that must not be blocked.

[RegisterDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registerducknotification) does not repeat the MTA note. Registration still goes through `IAudioSessionManager2`, whose using thread "must be initialized for COM" ([IAudioSessionManager2](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionmanager2), citing `CoInitializeEx`).

### Endpoint device events (`IMMNotificationClient`)

[IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient) / [Device Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-events): the MMDevice module calls the client's methods when device events occur. No MTA mandate, no "session manager thread" sentence. Callbacks are still treated as a system callback that must be nonblocking (below). The Device Events sample even calls `CoInitialize(NULL)` inside `_PrintDeviceName`, which is `COINIT_APARTMENTTHREADED` ([CoInitializeEx](https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex): `CoInitialize` is equivalent to `CoInitializeEx` with `COINIT_APARTMENTTHREADED`). That only makes sense if the callback thread is not already a well-defined STA UI thread.

### COM apartment rules those pages cite

[CoInitializeEx](https://learn.microsoft.com/en-us/windows/win32/api/combaseapi/nf-combaseapi-coinitializeex): STA objects receive calls only on their apartment thread, "serialized and arrive only at message-queue boundaries (when the PeekMessage or SendMessage function is called)." MTA objects "must be able to receive method calls from other threads at any time."

[Processes, Threads, and Apartments](https://learn.microsoft.com/en-us/windows/win32/com/processes--threads--and-apartments): STA (`COINIT_APARTMENTTHREADED`) when the thread has a message loop; MTA (`COINIT_MULTITHREADED`) for background work with no message loop. An STA thread **must** pump messages; a raw `WaitForSingleObject` on STA deadlocks incoming COM. MTA has at most one per process; STA can be many.

If a sink were an STA object, COM would marshal callbacks onto that STA via its message queue, and that thread would have to pump. Session-create docs do not take that path: they require a process MTA on a non-UI thread, or there are no session-create notifications at all.

## What must stay alive

`IAudioSessionManager2` is activated **per endpoint** (`IMMDevice::Activate` with `IID_IAudioSessionManager2`; [IAudioSessionManager2](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionmanager2)). A watch across capture and render devices is one manager per device, not one process-wide object.

| Object | Why it must stay |
| --- | --- |
| `IAudioSessionManager2` (per watched endpoint) | Registration target for session-create and ducking. The using thread must stay COM-initialized. |
| `IAudioSessionNotification` sink | [RegisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registersessionnotification) **AddRef**s on success; [UnregisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-unregistersessionnotification) **Release**s on success. |
| `IAudioSessionEnumerator` + `GetCount` | Required once after register so create notifications start ([RegisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registersessionnotification)). The enumerator may still miss sessions reported only through the callback; the app is told to keep its own list and **AddRef** each `IAudioSessionControl` ([IAudioSessionEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator)). |
| `IAudioSessionControl` from `OnSessionCreated` | [OnSessionCreated](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionnotification-onsessioncreated): "This method must take a reference to the session in the *NewSession* parameter if it wants to keep the reference after this call completes." |
| `IAudioSessionControl` and/or `IAudioSessionManager` after `RegisterAudioSessionNotification` | [RegisterAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-registeraudiosessionnotification): unregister before releasing the last ref on the events sink. Also: "the client should call UnregisterAudioSessionNotification before releasing all of its references to the IAudioSessionControl and IAudioSessionManager objects. Unless the client retains a reference to at least one of these two objects, the session manager leaks the storage that it allocated to hold the registration information. After registering a notification interface, the client continues to receive notifications for only as long as at least one of these two objects exists." |
| `IAudioSessionEvents` sink | Register **AddRef**s; Unregister **Release**s. Unregister before the client's last release, or the manager never drops its ref (deadlock if Unregister is in the destructor). |
| `IAudioVolumeDuckNotification` sink | Unregister **Release**s on success ([UnregisterDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-unregisterducknotification)); keep the sink until then. |
| `IMMNotificationClient` sink | [RegisterEndpointNotificationCallback](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-immdeviceenumerator-registerendpointnotificationcallback) / Unregister **do not** AddRef/Release. The client must keep the object alive until after Unregister returns. |
| `IMMDeviceEnumerator` | Needed to Unregister. The register/unregister remarks warn that dropping the client too early also leaks enumerator-held resources. |

[IAudioSessionEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator): "there is no expiration mechanism enforced by the audio system on the session control objects. A session control is valid as long as the application has a reference to the session control in the list."

## Unregister and in-flight teardown

Do not register or unregister from inside a callback:

- [IAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionnotification): "The application must not register or unregister notification callbacks during an event callback."
- [IAudioVolumeDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiovolumeducknotification): same sentence.
- [IAudioSessionEvents](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionevents): never call `UnregisterAudioSessionNotification` during an event callback.
- [IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient): never call Register/UnregisterEndpointNotificationCallback from a notification method.

[UnregisterDuckNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-unregisterducknotification): "After the application calls UnregisterDuckNotification, any pending events are not reported to the application."

[RegisterAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-registeraudiosessionnotification) / [UnregisterAudioSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessioncontrol-unregisteraudiosessionnotification): Unregister from the events-object destructor deadlocks, because Unregister waits for the manager's Release and the manager waits for Unregister.

The pages do not say that Unregister waits for an in-flight callback to finish. What they do say: never drop the **final** reference on a WASAPI object ([IAudioSessionEvents](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionevents)) or an MMDevice API object ([IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient)) **during** the callback. Combined with AddRef-on-register (session/duck/events sinks) or client-held refs (endpoint sink), the sink object is still alive while a callback runs. Destroying the GUI HWND while a callback is in `PostMessage` is a separate Win32 lifetime issue; Unregister first, from a non-callback thread, then tear down the window.

If the process tears down the GUI without Unregister, session/events/duck sinks leak because the manager still holds a ref; the endpoint sink can be deleted while MMDevice still calls it, because that API never AddRef'd.

## What is illegal on the callback

Documented as illegal / deadlock:

- Wait on a synchronization object during `IAudioSessionEvents` or `IMMNotificationClient` ([IAudioSessionEvents](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionevents), [IMMNotificationClient](https://learn.microsoft.com/en-us/windows/win32/api/mmdeviceapi/nn-mmdeviceapi-immnotificationclient): methods "must be nonblocking").
- Block the ducking notification thread ([Implementation Considerations for Ducking Notifications](https://learn.microsoft.com/en-us/windows/win32/coreaudio/handling-audio-ducking-events-from-communication-devices)).
- Register or unregister any of these notification interfaces from inside their callbacks (all four families, citations above).
- Release the final reference on a WASAPI object from `IAudioSessionEvents`, or on an MMDevice API object from `IMMNotificationClient`.

Not stated as illegal:

- Heap allocation. The pages never ban it.
- `PostMessage` (the official samples do this for session-create and ducking).
- Calling some MMDevice methods from `IMMNotificationClient`: the [Device Events](https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-events) sample calls `IMMDeviceEnumerator::GetDevice` / `OpenPropertyStore` from the callback. That is not a capture/render buffer callback.
- Holding `IAudioSessionControl*` from `OnSessionCreated` after **AddRef** ([OnSessionCreated](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionnotification-onsessioncreated)).

The docs never say "do not call back into WASAPI" as a blanket rule. They say do not wait, do not unregister, do not drop the last ref.

## GUI thread vs marshal

`src/gui/main.cpp` already pumps Win32 messages:

```cpp
while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
{
    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
}
```

That loop does not call `CoInitializeEx`. The ticket treats it as the STA UI thread. [Processes, Threads, and Apartments](https://learn.microsoft.com/en-us/windows/win32/com/processes--threads--and-apartments) says a UI thread with a message loop is the STA case (`COINIT_APARTMENTTHREADED`). [Single-Threaded Apartments](https://learn.microsoft.com/en-us/windows/win32/com/single-threaded-apartments) says each STA must have a GetMessage/DispatchMessage (or equivalent) loop so COM can deliver calls.

That pump is enough to **dispatch** `PostMessage` work and, if this thread is STA, to **deliver marshaled COM calls** to STA objects living on it.

It is **not** enough, by itself, for session-create notifications. [RegisterSessionNotification](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nf-audiopolicy-iaudiosessionmanager2-registersessionnotification) / [IAudioSessionEnumerator](https://learn.microsoft.com/en-us/windows/win32/api/audiopolicy/nn-audiopolicy-iaudiosessionenumerator): initialize MTA on a **non-UI** thread or the process does not receive session notifications; UI threads should be apartment-threaded. `OnSessionCreated` then runs on the session manager thread. The Learn sample marshals to the UI with `PostMessage`, it does not implement the sink as STA work on the GUI thread.

Ducking docs say the same split: notification thread in the background, window messages on the UI thread, do not block the notification thread.

So: the ImGui GUI thread can be the **recipient** of marshaled UI updates. It is the wrong apartment to **register** session-create notifications on, and the sink must not do UI work on the callback. This note does not pick the `AppUi` wiring (dedicated MTA worker vs some other MTA thread already in process, message vs other marshal).

## Not the capture/render audio thread

This repo's WASAPI pump is `WasapiStream::threadMain` (`src/core/WasapiStream.cpp`): `ComInitGuard` = `CoInitializeEx(nullptr, COINIT_MULTITHREADED)` (`src/core/ComUtil.h`), then `SetEventHandle` and `runLoop`. That is a worker this process created for capture/render.

Session-create callbacks are documented on the **session manager thread**, not that worker. Ducking is "asynchronously in the background." `IAudioSessionEvents` / `IMMNotificationClient` are callbacks from the WASAPI / MMDevice system module. None of those pages identify the client's `IAudioClient` event-driven pump.

The repo rule (capture/render callbacks must not heap-allocate or take blocking locks) therefore does not apply because these are the same thread — they are not. Blocking waits are still illegal on `IAudioSessionEvents` and `IMMNotificationClient` because **those** pages say the methods must be nonblocking. Heap allocation is not banned on the notification callbacks.

`LiveSessionEnumerator` today activates `IAudioSessionManager2`, enumerates, and drops the manager (`src/core/LiveSessionEnumerator.cpp`). That matches map #105: nothing is registered, so nothing currently has to stay alive across frames.
