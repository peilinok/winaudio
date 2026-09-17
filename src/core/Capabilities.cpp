#include "Capabilities.h"
#include "AudioFormat.h"
#include "FormatSpec.h"
namespace wa {
namespace {
bool sameCandidateFormat(const AudioFormat& a, const AudioFormat& b) {
    const uint32_t aMask = a.channelMask ? a.channelMask : defaultChannelMask(a.channels);
    const uint32_t bMask = b.channelMask ? b.channelMask : defaultChannelMask(b.channels);
    return a == b && aMask == bMask;
}

void appendCandidate(std::vector<FormatCandidate>& out, const AudioFormat& fmt,
                     FormatOrigin origin) {
    for (FormatCandidate& candidate : out) {
        if (sameCandidateFormat(candidate.fmt, fmt)) {
            candidate.origins |= formatOriginBit(origin);
            return;
        }
    }
    out.push_back(FormatCandidate{fmt, formatOriginBit(origin)});
}
} // namespace

std::vector<AudioFormat> allFormatCandidates() {
    static const uint32_t rates[] = {44100,48000,88200,96000,176400,192000};
    struct D { uint16_t bits; bool isFloat; };
    static const D depths[] = {{16,false},{24,false},{32,false},{32,true}};
    static const uint16_t chans[] = {1,2,4,6,8};
    std::vector<AudioFormat> out;
    out.reserve(120);
    for (uint32_t r : rates)
        for (const D& d : depths)
            for (uint16_t c : chans)
                out.push_back(AudioFormat{r, c, d.bits, d.isFloat});
    return out;
}

std::vector<FormatCandidate> capabilityCandidates(const DeviceCapabilities& caps) {
    std::vector<FormatCandidate> out;
    out.reserve(123);
    if (caps.hasMix) appendCandidate(out, caps.mixFormat, FormatOrigin::Mix);
    if (caps.hasDevice) appendCandidate(out, caps.deviceFormat, FormatOrigin::Device);
    if (caps.hasOem) appendCandidate(out, caps.oemFormat, FormatOrigin::Oem);
    for (const AudioFormat& fmt : allFormatCandidates())
        appendCandidate(out, fmt, FormatOrigin::Standard);
    return out;
}
std::vector<FormatSupport> buildCapabilityMatrix(
    const std::vector<FormatCandidate>& cands,
    const std::function<SupportLevel(const AudioFormat&)>& sharedPred,
    const std::function<SupportLevel(const AudioFormat&)>& exclusivePred) {
    std::vector<FormatSupport> m;
    m.reserve(cands.size());
    for (const FormatCandidate& candidate : cands) {
        m.push_back(FormatSupport{candidate.fmt, candidate.origins,
                                  sharedPred(candidate.fmt), exclusivePred(candidate.fmt)});
    }
    return m;
}
AudioFormat chooseDefaultFormat(
    BackendKind kind, const AudioFormat& mixFormat, const AudioFormat* deviceFormat,
    const std::vector<AudioFormat>& exclusiveCandidates,
    const std::function<bool(const AudioFormat&)>& exclusivePred) {
    if (kind == BackendKind::WasapiShared) return mixFormat;
    std::vector<AudioFormat> cands;
    if (deviceFormat) cands.push_back(*deviceFormat);
    for (const AudioFormat& c : exclusiveCandidates) cands.push_back(c);
    int idx = selectSupportedFormat(cands, exclusivePred);
    if (idx >= 0) return cands[(size_t)idx];
    return exclusiveCandidates.empty() ? mixFormat : exclusiveCandidates.front();
}
} // namespace wa
