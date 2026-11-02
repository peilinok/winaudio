#pragma once
#include <cstdint>

namespace wa::startup_probe {
struct Shared {
    uint32_t pid = 0;
    uint32_t crtEntered = 0;
    uint32_t mainEntered = 0;
    uint32_t calls = 0;
    long audioResult = 0;
    uintptr_t imageBase = 0;
    uint32_t entryRva = 0;
    uint8_t entryByte = 0;
    int64_t crtCounter = 0;
    int64_t firstCallCounter = 0;
    int64_t mainCounter = 0;
    uint32_t controllerStage = 0;
    uint32_t targetPid = 0;
    int64_t stageCounters[8]{};
    uint32_t sourceThread = 0;
    uintptr_t clientAddress = 0;
    long sourceResults[4]{};
    uint32_t sampleRate = 0;
    uint32_t bits = 0;
    uint32_t channels = 0;
    uint32_t releaseRowsBefore = 0;
    uint32_t releaseRowsAfter = 0;
    uint32_t uninitRowsAfter = 0;
};
}  // namespace wa::startup_probe
