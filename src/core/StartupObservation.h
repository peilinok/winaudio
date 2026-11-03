#pragma once
#include <cstdint>
#include <string>

namespace wa {
// Frozen evidence of the startup boundary, not a claim about future objects.
struct StartupObservation {
    uint32_t mainThreadId = 0;
    uintptr_t imageBase = 0, entryAddress = 0;
    uint32_t entryRva = 0, protocolLayout = 0;
    uint32_t acceptedAttempts = 0, rejectedAttempts = 0, capabilityError = 0;
    bool receiverReady = false;
    int64_t receiverReadyCounter = 0;
    std::string hookPath;
};
}
