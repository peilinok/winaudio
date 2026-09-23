#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "Result.h"

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

// Mono 16-bit phrases. Tests inject clips with set(). Production loads the
// bundled directory. Clips are copied into the track at create.
class PhraseCatalog {
public:
    void set(ChannelPhrase id, std::vector<int16_t> mono);
    const std::vector<int16_t>* find(ChannelPhrase id) const;

private:
    std::vector<int16_t> clips_[static_cast<int>(ChannelPhrase::Count)];
    bool present_[static_cast<int>(ChannelPhrase::Count)]{};
};

struct PhraseAsset {
    ChannelPhrase phrase = ChannelPhrase::None;
    const char* fileName = "";
    const char* reportName = "";
    bool projectOwned = false;
};

// One entry per spoken phrase. LFE is LFE.wav, never Rear_Center.wav.
const PhraseAsset* phraseAssets(size_t& count);

// Names of phrases whose file is missing or not mono 48 kHz 16-bit PCM.
// A missing project-owned recording is reported by reportName.
std::vector<std::string> missingProductionPhrases(const std::wstring& directory);

// Loads every file that passes the format check. Missing names are appended
// to missing when it is not null. Returns failure when any phrase is missing
// or the wrong format; clips that did load stay in the catalog.
Result loadProductionCatalog(PhraseCatalog& catalog, const std::wstring& directory,
                             std::vector<std::string>* missing);

} // namespace wa
