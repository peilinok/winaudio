#pragma once
#include <functional>
#include <cstdint>
#include <vector>
#include "AudioFormatDef.h"
#include "Engine.h"   // BackendKind
namespace wa {
enum class FormatOrigin : uint8_t {
    Standard = 1u << 0,
    Mix      = 1u << 1,
    Device   = 1u << 2,
    Oem      = 1u << 3,
};
using FormatOrigins = uint8_t;
inline constexpr FormatOrigins formatOriginBit(FormatOrigin origin) {
    return static_cast<FormatOrigins>(origin);
}
inline constexpr bool hasFormatOrigin(FormatOrigins origins, FormatOrigin origin) {
    return (origins & formatOriginBit(origin)) != 0;
}
struct FormatCandidate { AudioFormat fmt{}; FormatOrigins origins=0; };
enum class SupportLevel : uint8_t { Unsupported, ClosestMatch, Exact };
inline constexpr bool isSupported(SupportLevel support) {
    return support != SupportLevel::Unsupported;
}
struct FormatSupport {
    AudioFormat fmt{};
    FormatOrigins origins = 0;
    SupportLevel shared = SupportLevel::Unsupported;
    SupportLevel exclusive = SupportLevel::Unsupported;
};
struct DeviceCapabilities {
    AudioFormat mixFormat{}, deviceFormat{}, oemFormat{};
    bool hasMix=false, hasDevice=false, hasOem=false;
    std::vector<FormatSupport> matrix;
};
std::vector<AudioFormat> allFormatCandidates();
std::vector<FormatCandidate> capabilityCandidates(const DeviceCapabilities& caps);
std::vector<FormatSupport> buildCapabilityMatrix(
    const std::vector<FormatCandidate>& cands,
    const std::function<SupportLevel(const AudioFormat&)>& sharedPred,
    const std::function<SupportLevel(const AudioFormat&)>& exclusivePred);
AudioFormat chooseDefaultFormat(
    BackendKind kind, const AudioFormat& mixFormat, const AudioFormat* deviceFormat,
    const std::vector<AudioFormat>& exclusiveCandidates,
    const std::function<bool(const AudioFormat&)>& exclusivePred);
} // namespace wa
