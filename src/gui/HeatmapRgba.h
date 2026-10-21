#pragma once
#include <cstdint>
#include <cstddef>

namespace wa {

// One RGBA8 stop for a continuous spectrogram colormap (matches ImPlot continuous lerp).
struct HeatmapColorStop {
    float r, g, b, a;
};

// Pack little-endian R8G8B8A8 (DXGI_FORMAT_R8G8B8A8_UNORM / ImGui ImU32 on LE).
inline uint32_t packRgba8(float r, float g, float b, float a) {
    const auto q = [](float x) -> uint32_t {
        if (x <= 0.f) return 0;
        if (x >= 1.f) return 255;
        return static_cast<uint32_t>(x * 255.f + 0.5f);
    };
    return q(r) | (q(g) << 8) | (q(b) << 16) | (q(a) << 24);
}

// Build a 256-entry LUT from continuous color stops (same spacing as ImPlot AddColormap).
void buildHeatmapLut(const HeatmapColorStop* stops, int stopCount, uint32_t* lut256);

// Convert a rows*cols row-major dB grid into R8G8B8A8 pixels.
// scaleMin..scaleMax map to lut[0]..lut[255]. outRgba must hold rows*cols entries.
void fillHeatmapRgba(const float* values, int rows, int cols, float scaleMin, float scaleMax,
                     const uint32_t* lut256, uint32_t* outRgba);

}  // namespace wa