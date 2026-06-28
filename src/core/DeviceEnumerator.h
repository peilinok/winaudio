#pragma once
#include <vector>
#include "IAudioBackend.h"
#include "Result.h"

namespace wa {

class DeviceEnumerator {
public:
    Result enumerate(DataFlow flow, std::vector<DeviceInfo>& out);
    Result defaultDevice(DataFlow flow, DeviceInfo& out);
    Result mixFormat(const DeviceId& id, AudioFormat& out); // empty id = default render
};

} // namespace wa
