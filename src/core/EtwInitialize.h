#pragma once
#include "PipelineGraph.h"
#include "Result.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace wa {

struct EtwEventFields {
    uint32_t processId = 0;
    int64_t timeMs = 0;
    std::string deviceId;
    std::optional<std::string> category;
    std::optional<bool> raw;
    std::optional<bool> matchFormat;
    std::optional<bool> exclusive;
    std::optional<int32_t> hresult;
};

inline constexpr int64_t kEtwMatchWindowMs = 5000;

enum class EtwWatchStatus : uint8_t { Stopped, Listening, Unavailable };

const char* etwWatchStatusText(EtwWatchStatus status);

int64_t etwNowMs();

// Map a TDH / canned property dictionary onto fields. headerPid is used when
// no PID property is present. Does not EnableTrace.
EtwEventFields etwFieldsFromProperties(
    uint32_t headerPid,
    int64_t timeMs,
    const std::vector<std::pair<std::string, std::string>>& props);

// Join canned events onto one Live session. Match is PID + time window +
// parsed device id. No match leaves present=false (do not glue the latest
// Initialize on the machine).
EtwInitializeHint matchEtwInitialize(
    const LiveSessionView& session,
    const std::vector<EtwEventFields>& events,
    int64_t nowMs,
    int64_t windowMs = kEtwMatchWindowMs);

// Real-time ETW adapter. Enable failure is Unavailable; snapshot stays empty.
class EtwInitializeWatch {
public:
    EtwInitializeWatch();
    ~EtwInitializeWatch();
    EtwInitializeWatch(const EtwInitializeWatch&) = delete;
    EtwInitializeWatch& operator=(const EtwInitializeWatch&) = delete;

    Result start();
    void stop();
    EtwWatchStatus status() const;
    std::vector<EtwEventFields> snapshot() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wa
