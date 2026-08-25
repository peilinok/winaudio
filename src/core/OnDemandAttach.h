#pragma once
#include "HookedCall.h"
#include "Result.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wa {

inline constexpr bool kAttachLaunchSuspended = false;
inline constexpr bool kAttachAutoInject = false;

enum class AttachBlock : uint8_t {
    None,
    PidZero,
    SelfProcess,
    Audiodg,
    CrossBitness,
    NoDebugRights,
};

const char* attachBlockText(AttachBlock block);

// Pure policy. Tests use this seam; they do not inject.
AttachBlock evaluateAttach(uint32_t pid, uint32_t ourPid, bool sameBitness, bool hasDebugRights,
                           const std::string& processName);

class OnDemandAttach {
public:
    OnDemandAttach();
    ~OnDemandAttach();
    OnDemandAttach(const OnDemandAttach&) = delete;
    OnDemandAttach& operator=(const OnDemandAttach&) = delete;

    Result start(uint32_t pid);
    void stop();
    bool attached() const;
    uint32_t pid() const;
    AttachBlock lastBlock() const;
    std::vector<HookedCall> drain();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wa
