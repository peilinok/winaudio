#include "Capabilities.h"
#include "FormatSpec.h"
namespace wa {
std::vector<AudioFormat> allFormatCandidates() {
    static const uint32_t rates[] = {44100,48000,88200,96000,176400,192000};
    struct D { uint16_t bits; bool isFloat; };
    static const D depths[] = {{16,false},{24,false},{32,false},{32,true}};
    static const uint16_t chans[] = {1,2};
    std::vector<AudioFormat> out;
    out.reserve(48);
    for (uint32_t r : rates)
        for (const D& d : depths)
            for (uint16_t c : chans)
                out.push_back(AudioFormat{r, c, d.bits, d.isFloat});
    return out;
}
std::vector<FormatSupport> buildCapabilityMatrix(
    const std::vector<AudioFormat>& cands,
    const std::function<bool(const AudioFormat&)>& sharedPred,
    const std::function<bool(const AudioFormat&)>& exclusivePred) {
    std::vector<FormatSupport> m;
    m.reserve(cands.size());
    for (const AudioFormat& f : cands)
        m.push_back(FormatSupport{f, sharedPred(f), exclusivePred(f)});
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
