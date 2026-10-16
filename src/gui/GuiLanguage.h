#pragma once

#include <cstdint>

namespace wa::gui_language {

enum class Language { En, ZhCn };

// Win32 PRIMARYLANGID mask. LANG_CHINESE is 0x04, so simplified, traditional,
// and Hong Kong user-interface languages all select zh-CN. This is the user UI
// language id, not the regional-format locale.
inline bool uiLanguageIsChinese(std::uint16_t langId) {
    constexpr std::uint16_t kPrimaryMask = 0x3FFu;
    constexpr std::uint16_t kLangChinese = 0x04u;
    return (langId & kPrimaryMask) == kLangChinese;
}

inline const char* token(Language language) {
    return language == Language::ZhCn ? "zh-CN" : "en";
}

struct Resolved {
    bool logCollapsed = false;
    Language language = Language::En;
    bool fromOverride = false;
};

}  // namespace wa::gui_language
