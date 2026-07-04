#pragma once
#include <functional>
#include <vector>
#include "AudioFormatDef.h"
#include "Engine.h"   // BackendKind
namespace wa {
struct FormatSupport { AudioFormat fmt{}; bool sharedOk=false; bool exclusiveOk=false; };
struct DeviceCapabilities {
    AudioFormat mixFormat{}, deviceFormat{}, oemFormat{};
    bool hasMix=false, hasDevice=false, hasOem=false;
    std::vector<FormatSupport> matrix;
};
std::vector<AudioFormat> allFormatCandidates();
std::vector<FormatSupport> buildCapabilityMatrix(
    const std::vector<AudioFormat>& cands,
    const std::function<bool(const AudioFormat&)>& sharedPred,
    const std::function<bool(const AudioFormat&)>& exclusivePred);
AudioFormat chooseDefaultFormat(
    BackendKind kind, const AudioFormat& mixFormat, const AudioFormat* deviceFormat,
    const std::vector<AudioFormat>& exclusiveCandidates,
    const std::function<bool(const AudioFormat&)>& exclusivePred);
} // namespace wa
