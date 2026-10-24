#pragma once
#include "PipelineGraph.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace wa {

bool isPumpMethod(const std::string& method);

std::string sanitizeHookedArgs(std::string args);

// Decode hook_ipc arg blob into CallArg rows. Returns false when blob is empty
// or not a v1 payload; out is cleared either way.
bool decodeCallArgBlob(const uint8_t* data, uint16_t len, std::vector<CallArg>& out);

const char* callArgDirName(uint8_t dir);

inline constexpr size_t kDefaultPumpRingCap = 64;

struct PumpCall {
    uint64_t sequence = 0;
    HookedCall call;
};

struct CallLogView {
    std::vector<HookedCall> entries;
    uint32_t pumpXruns = 0;
};

// Control-path records are kept in full. Pump GetBuffer/ReleaseBuffer are
// omitted unless pumpEnabled; when on, only the last pumpCap pump rows remain.
// xrun counts include dropped pump records. Args never keep a pcm= payload.
CallLogView shapeCallLog(const std::vector<HookedCall>& in, bool pumpEnabled,
                         size_t pumpCap = kDefaultPumpRingCap);

EtwInitializeHint extractHookedInitialize(const std::vector<HookedCall>& calls);

// Latest Initialize (by timeMs) picks the primary streamId when non-zero; only that
// stream's Initialize / SetClientProperties / InitializeSharedAudioStream contribute.
struct HookedInitializeExtract {
    EtwInitializeHint hint;
    uint32_t primaryStreamId = 0;
    size_t streamCount = 0;  // distinct stream ids with Initialize* in the input
};

HookedInitializeExtract extractHookedInitializeDetailed(const std::vector<HookedCall>& calls);
HookedInitializeExtract extractHookedInitializeRefs(const std::vector<const HookedCall*>& calls);

// Hooked fields replace matching ETW fields when present.
EtwInitializeHint mergeInitializeHint(const EtwInitializeHint& etw,
                                      const EtwInitializeHint& hooked);

// Keep calls whose pid is 0 (legacy / unset) or equals sessionPid.
std::vector<HookedCall> filterHookedCallsForSession(const std::vector<HookedCall>& calls,
                                                    uint32_t sessionPid);

enum class InitializeFieldSource : uint8_t { None = 0, Etw = 1, Hooked = 2 };

// Honest join summary for Pipeline graph banners (device-free).
struct PipelineJoinStatus {
    InitializeFieldSource initializeSource = InitializeFieldSource::None;
    bool etwMatched = false;
    bool probesPresent = false;
    size_t hookedStreamCount = 0;
    uint32_t primaryStreamId = 0;
    size_t sessionControlCalls = 0;
    size_t otherPidCalls = 0;
};

PipelineJoinStatus makePipelineJoinStatus(const EtwInitializeHint& etw,
                                          const std::vector<ProbeSlice>& probes,
                                          const std::vector<HookedCall>& allCalls,
                                          const std::vector<HookedCall>& sessionCalls);

const char* initializeFieldSourceName(InitializeFieldSource src);

}  // namespace wa

