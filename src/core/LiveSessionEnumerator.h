#pragma once
#include <vector>
#include "PipelineGraph.h"
#include "Result.h"

namespace wa {

class LiveSessionEnumerator {
public:
    Result enumerate(std::vector<LiveSessionView>& out);
};

}  // namespace wa
