#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "AudioFormat.h"

namespace wa {

bool parseFormatSpec(const std::string& spec, AudioFormat& out);
long long alignedBufferDuration100ns(uint32_t sampleRate, uint32_t alignedFrames);
int  selectSupportedFormat(const std::vector<AudioFormat>& candidates,
                           const std::function<bool(const AudioFormat&)>& isSupported);
std::vector<AudioFormat> defaultExclusiveCaptureCandidates();

} // namespace wa
