#pragma once
#include "IAudioBackend.h"
#include "Result.h"

namespace wa {

// Process-wide gate: WasapiStream start/stop adjust the count so endpoint SysFx
// writes refuse while any capture/render stream in this process is running.
void noteAppStreamStarted();
void noteAppStreamStopped();
bool anyAppStreamActive();

// Refuse endpoint SysFx changes while this app has an active capture/render stream.
// Message matches the product copy: "start stream 前才能改".
inline Result refuseEnhancementsWhileStreaming() {
    return Result::Fail(static_cast<long>(0x80004004L) /* E_ABORT */,
                        "start stream 前才能改");
}

// Write PKEY_AudioEndpoint_Disable_SysFx via undocumented IPolicyConfig to BOTH
// the effects (FxProperties) store and the endpoint property store.
// Enabled -> ulVal 0 (ENDPOINT_SYSFX_ENABLED); Disabled -> ulVal 1 (ENDPOINT_SYSFX_DISABLED).
// Unknown is rejected. Does not elevate; ACCESS_DENIED is returned to the caller.
// When outInfo is non-null, re-reads DeviceInfo after a successful write.
// Audible APO change may require clients to reopen Shared streams (Sound CPL Apply
// typically forces a rebuild; a bare property write does not).
Result setEndpointEnhancements(DataFlow flow, const DeviceId& id, EnhancementsState state,
                               DeviceInfo* outInfo = nullptr);

}  // namespace wa