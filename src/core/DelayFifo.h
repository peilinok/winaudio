#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace wa {

// Frame-domain (interleaved float32) FIFO with drift controller.
// A "frame" is one sample-time across all channels.
// The drift controller holds EMA-smoothed occupancy near targetFrames by
// dropping or duplicating single frames (with a short crossfade) — no resampler.
class DelayFifo {
public:
    DelayFifo(size_t channels, size_t targetFrames, size_t capacityFrames, size_t deadbandFrames);

    // Producer: append interleaved frames. If full, oldest frames are discarded.
    void pushFrames(const float* interleaved, size_t frames);

    // Consumer: apply drift controller, then copy up to maxFrames frames into out.
    // Returns the number of frames written.
    size_t popFrames(float* interleaved, size_t maxFrames);

    size_t   fillFrames()        const;  // instantaneous occupancy (frames)
    double   lowpassFillFrames() const;  // EMA-smoothed occupancy
    uint64_t driftFixes()        const;  // count of drop/dup corrections

private:
    size_t channels_;
    size_t targetFrames_;
    size_t capacityFrames_;
    size_t deadbandFrames_;

    std::vector<float> buf_;   // size = capacityFrames * channels
    size_t head_  = 0;         // read position (frames, wraps mod capacityFrames_)
    size_t tail_  = 0;         // write position (frames, wraps mod capacityFrames_)
    size_t fill_  = 0;         // current occupancy (frames)

    double   lowpassFill_ = 0.0;
    uint64_t driftFixes_  = 0;

    std::vector<float> lastFrame_;  // last frame written to caller (channels floats)

    static constexpr double kAlpha           = 0.05;
    static constexpr size_t kCrossFadeFrames = 32;
};

} // namespace wa
