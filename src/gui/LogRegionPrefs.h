#pragma once

#include "GuiLanguage.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace wa::log_region_prefs {

inline constexpr const char* kFileName = "winaudio.ui.ini";

enum class LoadKind { Missing, Ok, Invalid };

struct LoadResult {
    bool collapsed = false;
    LoadKind kind = LoadKind::Missing;
    bool hasLanguage = false;
    wa::gui_language::Language language = wa::gui_language::Language::En;
};

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

inline bool sectionCounts(std::string_view name) {
    return name == "gui";
}

inline LoadResult parse(std::string_view text) {
    LoadResult r;
    r.kind = LoadKind::Invalid;
    bool foundLog = false;
    bool countKeys = true;
    std::string chunk(text);
    std::istringstream in(chunk);
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view t = trim(line);
        if (t.empty() || t.front() == '#' || t.front() == ';') continue;
        if (t.front() == '[') {
            const auto end = t.find(']');
            if (end == std::string_view::npos) {
                countKeys = false;
                continue;
            }
            countKeys = sectionCounts(trim(t.substr(1, end - 1)));
            continue;
        }
        if (!countKeys) continue;
        const auto eq = t.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(t.substr(0, eq));
        const std::string_view value = trim(t.substr(eq + 1));
        if (key == "log_collapsed") {
            if (value == "0" || value == "1") {
                r.collapsed = (value == "1");
                foundLog = true;
            }
            continue;
        }
        if (key != "language") continue;
        if (value == "en") {
            r.language = wa::gui_language::Language::En;
            r.hasLanguage = true;
        } else if (value == "zh-CN") {
            r.language = wa::gui_language::Language::ZhCn;
            r.hasLanguage = true;
        } else {
            r.hasLanguage = false;
        }
    }
    if (foundLog || r.hasLanguage) r.kind = LoadKind::Ok;
    return r;
}

inline wa::gui_language::Resolved resolve(const LoadResult& loaded, bool displayIsChinese) {
    wa::gui_language::Resolved out;
    out.logCollapsed = loaded.collapsed;
    if (loaded.hasLanguage) {
        out.language = loaded.language;
        out.fromOverride = true;
        return out;
    }
    out.language = displayIsChinese ? wa::gui_language::Language::ZhCn
                                    : wa::gui_language::Language::En;
    out.fromOverride = false;
    return out;
}

inline LoadResult load(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
        return {};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {false, LoadKind::Invalid};
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in && !in.eof())
        return {false, LoadKind::Invalid};
    return parse(ss.str());
}

inline std::string serialize(bool collapsed, const wa::gui_language::Language* language) {
    std::string out = "[gui]\n";
    out += collapsed ? "log_collapsed=1\n" : "log_collapsed=0\n";
    if (language != nullptr) {
        out += "language=";
        out += wa::gui_language::token(*language);
        out += "\n";
    }
    return out;
}

inline bool writeFile(const std::filesystem::path& path, const std::string& body) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << body;
    out.flush();
    return static_cast<bool>(out);
}

inline bool save(const std::filesystem::path& path, bool collapsed) {
    const wa::gui_language::Language* keep = nullptr;
    wa::gui_language::Language stored = wa::gui_language::Language::En;
    std::error_code ec;
    if (std::filesystem::exists(path, ec) && !ec) {
        const LoadResult prior = load(path);
        if (prior.hasLanguage) {
            stored = prior.language;
            keep = &stored;
        }
    }
    return writeFile(path, serialize(collapsed, keep));
}

inline bool saveLanguage(const std::filesystem::path& path,
                         wa::gui_language::Language language,
                         bool collapsed) {
    return writeFile(path, serialize(collapsed, &language));
}

}  // namespace wa::log_region_prefs
