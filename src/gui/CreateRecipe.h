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

inline std::vector<AudioFormat> sharedCandidates(const DeviceCapabilities& caps) {
    std::vector<AudioFormat> out;
    for (const auto& fs : caps.matrix) {
        if (fs.sharedOk) out.push_back(fs.fmt);
    }
    return out;
}

} // namespace wa::create_recipe
