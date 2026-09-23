#pragma once
#include <cstdint>
#include <vector>

namespace wa {

// Spoken identity of one Render Track channel. None means the row has no phrase.
enum class ChannelPhrase : uint8_t {
    None = 0,
    FrontLeft,
    FrontRight,
    FrontCenter,
    LowFrequency,
    BackLeft,
    BackRight,
    BackCenter,
    SideLeft,
    SideRight,
    FrontLeftOfCenter,
    FrontRightOfCenter,
    TopCenter,
    TopFrontLeft,
    TopFrontCenter,
    TopFrontRight,
    TopBackLeft,
    TopBackCenter,
    TopBackRight,
    Channel1,
    Channel2,
    Channel3,
    Channel4,
    Channel5,
    Channel6,
    Channel7,
    Channel8,
    Count
};

// Slot 0 is the lowest set mask bit. An empty mask uses "channel N" for slots 0..7
// (spoken as channel 1..8) and None above that.
ChannelPhrase phraseForSlot(uint32_t channelMask, uint16_t slot);

// Mono 16-bit phrases injected by tests. Production stays empty until the
// shipped catalog exists. Clips are copied into the track at create.
class PhraseCatalog {
public:
    void set(ChannelPhrase id, std::vector<int16_t> mono);
    const std::vector<int16_t>* find(ChannelPhrase id) const;

private:
    std::vector<int16_t> clips_[static_cast<int>(ChannelPhrase::Count)];
    bool present_[static_cast<int>(ChannelPhrase::Count)]{};
};

} // namespace wa
