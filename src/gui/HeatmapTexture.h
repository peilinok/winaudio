#pragma once
#include "HeatmapRgba.h"
#include "imgui.h"
#include <cstdint>
#include <array>
#include <vector>

struct ID3D11ShaderResourceView;
struct ID3D11Texture2D;
struct ID3D11Device;

namespace wa {
class FrameProbe;

// GPU heatmap for one spectrogram: rebuild RGBA + upload only when revision/dims change.
// Draw with ImPlot::PlotImage using textureId().
class HeatmapTexture {
public:
    using ResourceCreator = long (*)(ID3D11Device*, int, int,
                                    ID3D11Texture2D**, ID3D11ShaderResourceView**);
    explicit HeatmapTexture(ResourceCreator creator = nullptr) : creator_(creator) {}
    ~HeatmapTexture() { release(); }
    HeatmapTexture(const HeatmapTexture&) = delete;
    HeatmapTexture& operator=(const HeatmapTexture&) = delete;
    HeatmapTexture(HeatmapTexture&&) = delete;
    HeatmapTexture& operator=(HeatmapTexture&&) = delete;

    void release();

    // Ensure GPU texture matches values. Returns false if device missing or create failed.
    // lut256 is a 256-entry R8G8B8A8 table (build once from the spectrogram colormap).
    // A supplied probe is borrowed and must outlive this texture.
    bool ensure(const float* values, int rows, int cols, uint64_t revision, float scaleMin,
                float scaleMax, const uint32_t* lut256, FrameProbe* probe = nullptr,
                uint64_t source = 0, int direction = -1, int channel = -1);

    ImTextureID textureId() const;
    bool ready() const { return srv_ != nullptr; }

private:
    bool createTexture(int rows, int cols);
    void upload();

    ID3D11Texture2D* tex_ = nullptr;
    ID3D11ShaderResourceView* srv_ = nullptr;
    ID3D11Device* device_ = nullptr;  // borrowed; tex_ retains its creating device
    std::vector<uint32_t> rgba_;
    int rows_ = 0;
    int cols_ = 0;
    uint64_t revision_ = 0;
    bool haveRevision_ = false;
    float scaleMin_ = 0, scaleMax_ = 0;
    std::array<uint32_t, 256> lut_{};
    ResourceCreator creator_ = nullptr;
    FrameProbe* probe_ = nullptr;
    uint64_t source_ = 0;
    int direction_ = -1, channel_ = -1;
    bool resourceCounted_ = false;
};

}  // namespace wa
