#pragma once
#include "AudioFormat.h"
#include "Capabilities.h"
#include "FormatSpec.h"
#include "StreamParams.h"
#include <string>
#include <vector>

namespace wa::create_recipe {

// Create-time format selection for one page. System default means no requested
// format; a candidate or successful Custom Apply sets haveRequested.
struct FormatState {
    AudioFormat selected{};
    bool haveRequested = false;
    int choiceIdx = 0; // 0 = System default, -1 = custom, 1..n = candidates
    char custom[32] = "48000/16/2";
};

struct CreateRecipe {
    FormatState format{};
    StreamParams params{};
    DeviceCapabilities caps{};
    int deviceShown = -1;
    std::wstring deviceId{};
};

inline const AudioFormat* requestedOrNull(const FormatState& st) {
    return st.haveRequested ? &st.selected : nullptr;
}

inline void selectDefault(FormatState& st, const AudioFormat& mix = {}) {
    st.choiceIdx = 0;
    st.haveRequested = false;
    st.selected = mix;
}

inline void selectCandidate(FormatState& st, const AudioFormat& fmt, int comboIndex) {
    st.choiceIdx = comboIndex;
    st.haveRequested = true;
    st.selected = fmt;
}

inline bool applyCustom(FormatState& st, const char* text, std::string* error) {
    AudioFormat parsed{};
    if (!text || !parseFormatSpec(text, parsed)) {
        if (error) *error = "invalid format";
        return false;
    }
    st.selected = parsed;
    st.haveRequested = true;
    st.choiceIdx = -1;
    return true;
}

inline bool updateFormatDevice(CreateRecipe& recipe, int deviceShown,
                               const std::wstring& deviceId,
                               const DeviceCapabilities& caps,
                               const AudioFormat& defaultDisplay) {
    const bool changed = recipe.deviceId != deviceId ||
        (recipe.deviceShown >= 0 && recipe.deviceId.empty() && deviceId.empty() &&
         recipe.deviceShown != deviceShown);
    recipe.caps = caps;
    recipe.deviceShown = deviceShown;
    recipe.deviceId = deviceId;
    if (changed) selectDefault(recipe.format, defaultDisplay);
    return changed;
}

inline bool hasDeviceFormatOrigin(const FormatSupport& support) {
    return hasFormatOrigin(support.origins, FormatOrigin::Mix) ||
           hasFormatOrigin(support.origins, FormatOrigin::Device) ||
           hasFormatOrigin(support.origins, FormatOrigin::Oem);
}

inline std::vector<FormatSupport> sharedFormatChoices(const DeviceCapabilities& caps) {
    std::vector<FormatSupport> out;
    for (const auto& fs : caps.matrix) {
        if (isSupported(fs.shared) && hasDeviceFormatOrigin(fs))
            out.push_back(fs);
    }
    if (!caps.hasMix) return out;
    for (const auto& fs : caps.matrix) {
        if (isSupported(fs.shared) && !hasDeviceFormatOrigin(fs) &&
            hasFormatOrigin(fs.origins, FormatOrigin::Standard) &&
            fs.fmt.sampleRate == caps.mixFormat.sampleRate)
            out.push_back(fs);
    }
    return out;
}

inline std::vector<AudioFormat> sharedCandidates(const DeviceCapabilities& caps) {
    std::vector<AudioFormat> out;
    for (const auto& fs : caps.matrix) {
        if (isSupported(fs.shared)) out.push_back(fs.fmt);
    }
    return out;
}

} // namespace wa::create_recipe
