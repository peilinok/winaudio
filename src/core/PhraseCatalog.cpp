#include "PhraseCatalog.h"
#include "WavFile.h"
#include <filesystem>

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

namespace {
const PhraseAsset kAssets[] = {
    {ChannelPhrase::FrontLeft, "Front_Left.wav", "Front left", false},
    {ChannelPhrase::FrontRight, "Front_Right.wav", "Front right", false},
    {ChannelPhrase::FrontCenter, "Front_Center.wav", "Front center", false},
    {ChannelPhrase::BackLeft, "Rear_Left.wav", "Back left", false},
    {ChannelPhrase::BackRight, "Rear_Right.wav", "Back right", false},
    {ChannelPhrase::BackCenter, "Rear_Center.wav", "Back center", false},
    {ChannelPhrase::SideLeft, "Side_Left.wav", "Side left", false},
    {ChannelPhrase::SideRight, "Side_Right.wav", "Side right", false},
    {ChannelPhrase::LowFrequency, "LFE.wav", "LFE", true},
    {ChannelPhrase::FrontLeftOfCenter, "Front_Left_Of_Center.wav", "Front Left of Center", true},
    {ChannelPhrase::FrontRightOfCenter, "Front_Right_Of_Center.wav", "Front Right of Center", true},
    {ChannelPhrase::TopCenter, "Top_Center.wav", "Top Center", true},
    {ChannelPhrase::TopFrontLeft, "Top_Front_Left.wav", "Top Front Left", true},
    {ChannelPhrase::TopFrontCenter, "Top_Front_Center.wav", "Top Front Center", true},
    {ChannelPhrase::TopFrontRight, "Top_Front_Right.wav", "Top Front Right", true},
    {ChannelPhrase::TopBackLeft, "Top_Back_Left.wav", "Top Back Left", true},
    {ChannelPhrase::TopBackCenter, "Top_Back_Center.wav", "Top Back Center", true},
    {ChannelPhrase::TopBackRight, "Top_Back_Right.wav", "Top Back Right", true},
    {ChannelPhrase::Channel1, "Channel_1.wav", "channel 1", true},
    {ChannelPhrase::Channel2, "Channel_2.wav", "channel 2", true},
    {ChannelPhrase::Channel3, "Channel_3.wav", "channel 3", true},
    {ChannelPhrase::Channel4, "Channel_4.wav", "channel 4", true},
    {ChannelPhrase::Channel5, "Channel_5.wav", "channel 5", true},
    {ChannelPhrase::Channel6, "Channel_6.wav", "channel 6", true},
    {ChannelPhrase::Channel7, "Channel_7.wav", "channel 7", true},
    {ChannelPhrase::Channel8, "Channel_8.wav", "channel 8", true},
};

Result readPhraseWav(const std::filesystem::path& path, std::vector<int16_t>& samples) {
    WavReader reader;
    Result opened = reader.open(path.wstring());
    if (!opened) return opened;
    const AudioFormat& fmt = reader.format();
    if (fmt.sampleRate != 48000 || fmt.bitsPerSample != 16 || fmt.channels != 1 || fmt.isFloat) {
        reader.close();
        return Result::Fail(-1, "phrase must be 48000 Hz 16-bit mono");
    }
    samples.clear();
    int16_t buf[2048];
    for (;;) {
        const size_t bytes = reader.read(buf, sizeof(buf));
        if (bytes == 0) break;
        if (bytes % sizeof(int16_t) != 0) {
            reader.close();
            return Result::Fail(-1, "phrase WAV data is not 16-bit aligned");
        }
        samples.insert(samples.end(), buf, buf + bytes / sizeof(int16_t));
    }
    reader.close();
    if (samples.empty()) return Result::Fail(-1, "phrase WAV has no samples");
    return Result::Ok();
}
} // namespace

const PhraseAsset* phraseAssets(size_t& count) {
    count = sizeof(kAssets) / sizeof(kAssets[0]);
    return kAssets;
}

std::vector<std::string> missingProductionPhrases(const std::wstring& directory) {
    std::vector<std::string> missing;
    const std::filesystem::path root(directory);
    size_t count = 0;
    const PhraseAsset* assets = phraseAssets(count);
    for (size_t i = 0; i < count; ++i) {
        const std::filesystem::path file = root / assets[i].fileName;
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file, ec)) {
            missing.emplace_back(assets[i].reportName);
            continue;
        }
        std::vector<int16_t> samples;
        if (!readPhraseWav(file, samples)) missing.emplace_back(assets[i].reportName);
    }
    return missing;
}

Result loadProductionCatalog(PhraseCatalog& catalog, const std::wstring& directory,
                             std::vector<std::string>* missing) {
    if (missing) missing->clear();
    const std::vector<std::string> gaps = missingProductionPhrases(directory);
    if (missing) *missing = gaps;
    const std::filesystem::path root(directory);
    size_t count = 0;
    const PhraseAsset* assets = phraseAssets(count);
    for (size_t i = 0; i < count; ++i) {
        const std::filesystem::path file = root / assets[i].fileName;
        std::vector<int16_t> samples;
        if (!readPhraseWav(file, samples)) continue;
        catalog.set(assets[i].phrase, std::move(samples));
    }
    if (!gaps.empty()) return Result::Fail(-1, "channel ident recordings are missing");
    return Result::Ok();
}

} // namespace wa
