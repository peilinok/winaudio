#include "HeatmapRgba.h"
#include <algorithm>

namespace wa {
namespace {

HeatmapColorStop lerpStop(const HeatmapColorStop& a, const HeatmapColorStop& b, float t) {
    HeatmapColorStop o;
    o.r = a.r + (b.r - a.r) * t;
    o.g = a.g + (b.g - a.g) * t;
    o.b = a.b + (b.b - a.b) * t;
    o.a = a.a + (b.a - a.a) * t;
    return o;
}

}  // namespace

void buildHeatmapLut(const HeatmapColorStop* stops, int stopCount, uint32_t* lut256) {
    if (!stops || stopCount <= 0 || !lut256) return;
    if (stopCount == 1) {
        const uint32_t c = packRgba8(stops[0].r, stops[0].g, stops[0].b, stops[0].a);
        for (int i = 0; i < 256; ++i) lut256[i] = c;
        return;
    }
    for (int i = 0; i < 256; ++i) {
        const float t = static_cast<float>(i) / 255.f;
        const float x = t * static_cast<float>(stopCount - 1);
        const int lo = std::min(stopCount - 2, static_cast<int>(x));
        const float f = x - static_cast<float>(lo);
        const HeatmapColorStop s = lerpStop(stops[lo], stops[lo + 1], f);
        lut256[i] = packRgba8(s.r, s.g, s.b, s.a);
    }
}

void fillHeatmapRgba(const float* values, int rows, int cols, float scaleMin, float scaleMax,
                     const uint32_t* lut256, uint32_t* outRgba) {
    if (!values || !lut256 || !outRgba || rows <= 0 || cols <= 0) return;
    const float span = scaleMax - scaleMin;
    const int n = rows * cols;
    if (span <= 0.f) {
        const uint32_t c = lut256[0];
        for (int i = 0; i < n; ++i) outRgba[i] = c;
        return;
    }
    const float inv = 255.f / span;
    for (int i = 0; i < n; ++i) {
        float t = (values[i] - scaleMin) * inv;
        if (t <= 0.f) {
            outRgba[i] = lut256[0];
        } else if (t >= 255.f) {
            outRgba[i] = lut256[255];
        } else {
            outRgba[i] = lut256[static_cast<int>(t)];
        }
    }
}

}  // namespace wa