#pragma once
#include "AppUiText.h"
#include "imgui.h"

namespace wa::gui_font {

inline const ImWchar* ranges() {
    static const ImVector<ImWchar> ranges = [] {
        ImFontAtlas atlas;
        ImFontGlyphRangesBuilder builder;
        builder.AddRanges(atlas.GetGlyphRangesChineseSimplifiedCommon());
        for (const auto& row : ui_text::kCatalog) {
            builder.AddText(row.en);
            if (row.zh) builder.AddText(row.zh);
        }
        ImVector<ImWchar> result;
        builder.BuildRanges(&result);
        return result;
    }();
    return ranges.Data;
}

}  // namespace wa::gui_font
