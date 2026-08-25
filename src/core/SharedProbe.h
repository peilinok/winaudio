#pragma once
#include <string>
#include <vector>
#include "IAudioBackend.h"
#include "PipelineGraph.h"
#include "Result.h"
#include "StreamParams.h"

namespace wa {

struct SharedProbeRecipe {
    std::string label;
    bool raw = false;
    StreamParams params;
};

std::vector<SharedProbeRecipe> sharedProbeRecipes();

class SharedProbeHost {
public:
    virtual ~SharedProbeHost() = default;
    virtual Result open(const StreamParams& params) = 0;
    virtual Result readEffects(std::vector<AdvertisedEffect>& out) = 0;
    virtual void close() = 0;
};

// exclusive=true fails without calling the host. Recipe open failures still
// append a slice so the caller can keep the Live session radar.
Result runSharedProbes(SharedProbeHost& host, bool exclusive, std::vector<ProbeSlice>& out);

Result probeEndpointShared(DataFlow flow, const DeviceId& id, std::vector<ProbeSlice>& out);

}  // namespace wa
