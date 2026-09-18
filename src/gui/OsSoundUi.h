#pragma once
#include "AppUiText.h"
#include <cstdint>

namespace wa::os_sound_ui {

enum class Page {
    Monitor,
    Loopback,
    ApplicationLoopback,
};

enum class Button {
    Mmsys,
    MsSettings,
};

struct LaunchRecipe {
    const wchar_t* file;
    const wchar_t* params;
    const char* label;
    const char* failureLog;
};

inline LaunchRecipe recipe(Page page, Button button) {
    if (button == Button::MsSettings) {
        return {L"ms-settings:sound", L"", wa::ui_text::kMsSettings,
                wa::ui_text::kMsSettingsFailed};
    }
    const wchar_t* params =
        (page == Page::Monitor) ? L"mmsys.cpl,,1" : L"mmsys.cpl,,0";
    return {L"control.exe", params, wa::ui_text::kMmsysCpl,
            wa::ui_text::kMmsysCplFailed};
}

inline bool shellExecuteFailed(std::intptr_t code) {
    return code <= 32;
}

} // namespace wa::os_sound_ui
