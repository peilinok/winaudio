#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Result.h"

namespace wa {

struct AudioSessionProcess {
    uint32_t processId = 0;
    std::wstring processName;
};

void sortAndDedupeAudioSessionProcesses(std::vector<AudioSessionProcess>& rows);
bool parseApplicationLoopbackPid(const char* text, uint32_t& pidOut);

class AudioSessionEnumerator {
public:
    Result enumerate(std::vector<AudioSessionProcess>& out);
};

} // namespace wa
