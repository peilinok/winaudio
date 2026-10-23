#pragma once
#include "IAudioBackend.h"
#include "Result.h"
#include <cstdint>
#include <optional>

namespace wa {

// Undocumented IPolicyConfig (CLSID PolicyConfigClient). Same PKEY can live in the
// endpoint property store (bFxStore=false) and the effects/FxProperties store
// (bFxStore=true). Sound CPL / Win11 "Audio enhancements" write the Fx store;
// IMMDevice::OpenPropertyStore only sees the endpoint store.

// Read Disable_SysFx. Prefer Fx store, then endpoint store. nullopt = both empty
// / unreadable (caller treats as Enabled by Windows default).
std::optional<uint32_t> readDisableSysFxUlVal(const DeviceId& endpointId);

// Write Disable_SysFx to BOTH stores (Fx first, then endpoint) so APO enablement
// and our OpenPropertyStore-based readers stay aligned with Sound CPL.
Result writeDisableSysFxUlVal(const DeviceId& endpointId, uint32_t ulVal);

}  // namespace wa