#include "PhraseCatalog.h"

namespace wa {

namespace {
ChannelPhrase phraseForSpeakerBit(int bit) {
    switch (bit) {
    case 0: return ChannelPhrase::FrontLeft;
    case 1: return ChannelPhrase::FrontRight;
    case 2: return ChannelPhrase::FrontCenter;
    case 3: return ChannelPhrase::LowFrequency;
    case 4: return ChannelPhrase::BackLeft;
    case 5: return ChannelPhrase::BackRight;
    case 6: return ChannelPhrase::FrontLeftOfCenter;
    case 7: return ChannelPhrase::FrontRightOfCenter;
    case 8: return ChannelPhrase::BackCenter;
    case 9: return ChannelPhrase::SideLeft;
    case 10: return ChannelPhrase::SideRight;
    case 11: return ChannelPhrase::TopCenter;
    case 12: return ChannelPhrase::TopFrontLeft;
    case 13: return ChannelPhrase::TopFrontCenter;
    case 14: return ChannelPhrase::TopFrontRight;
    case 15: return ChannelPhrase::TopBackLeft;
    case 16: return ChannelPhrase::TopBackCenter;
    case 17: return ChannelPhrase::TopBackRight;
    default: return ChannelPhrase::None;
    }
}
} // namespace

ChannelPhrase phraseForSlot(uint32_t channelMask, uint16_t slot) {
    if (channelMask == 0) {
        if (slot >= 8) return ChannelPhrase::None;
        return static_cast<ChannelPhrase>(static_cast<int>(ChannelPhrase::Channel1) + slot);
    }
    uint16_t seen = 0;
    for (int bit = 0; bit < 32; ++bit) {
        if ((channelMask & (1u << bit)) == 0) continue;
        if (seen == slot) return phraseForSpeakerBit(bit);
        ++seen;
    }
    return ChannelPhrase::None;
}

void PhraseCatalog::set(ChannelPhrase id, std::vector<int16_t> mono) {
    const int index = static_cast<int>(id);
    if (index <= 0 || index >= static_cast<int>(ChannelPhrase::Count) || mono.empty()) return;
    clips_[index] = std::move(mono);
    present_[index] = true;
}

const std::vector<int16_t>* PhraseCatalog::find(ChannelPhrase id) const {
    const int index = static_cast<int>(id);
    if (index <= 0 || index >= static_cast<int>(ChannelPhrase::Count) || !present_[index])
        return nullptr;
    return &clips_[index];
}

} // namespace wa
