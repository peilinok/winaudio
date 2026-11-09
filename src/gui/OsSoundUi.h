#pragma once
#include "AppUiText.h"
#include <cstdint>

namespace wa::os_sound_ui {

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

inline LaunchRecipe recipe(Button button) {
    if (button == Button::MsSettings) {
        return {L"ms-settings:sound", L"", wa::ui_text::kMsSettings,
                wa::ui_text::kMsSettingsFailed};
    }
    return {L"control.exe", L"mmsys.cpl,,0", wa::ui_text::kMmsysCpl,
            wa::ui_text::kMmsysCplFailed};
}

inline bool shellExecuteFailed(std::intptr_t code) {
    return code <= 32;
}

} // namespace wa::os_sound_ui
