#pragma once
#include "IAudioBackend.h"
#include "PipelineGraph.h"
#include "Result.h"

namespace wa {

class EndpointGraphReader {
public:
    Result snapshot(DataFlow flow, const DeviceId& id, EndpointSnapshot& out);
};

}  // namespace wa
