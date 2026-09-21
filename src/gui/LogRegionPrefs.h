#pragma once

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
};

inline std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

inline std::string serialize(bool collapsed) {
    return collapsed ? "log_collapsed=1\n" : "log_collapsed=0\n";
}

inline LoadResult parse(std::string_view text) {
    LoadResult r;
    r.kind = LoadKind::Invalid;
    bool found = false;
    std::string chunk(text);
    std::istringstream in(chunk);
    std::string line;
    while (std::getline(in, line)) {
        const std::string_view t = trim(line);
        if (t.empty()) continue;
        const auto eq = t.find('=');
        if (eq == std::string_view::npos) continue;
        const std::string_view key = trim(t.substr(0, eq));
        const std::string_view value = trim(t.substr(eq + 1));
        if (key != "log_collapsed") continue;
        if (value == "0" || value == "1") {
            r.collapsed = (value == "1");
            found = true;
        }
    }
    if (found) r.kind = LoadKind::Ok;
    return r;
}

inline LoadResult load(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec)
        return {false, LoadKind::Missing};
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {false, LoadKind::Invalid};
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in && !in.eof())
        return {false, LoadKind::Invalid};
    return parse(ss.str());
}

inline bool save(const std::filesystem::path& path, bool collapsed) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    out << serialize(collapsed);
    out.flush();
    return static_cast<bool>(out);
}

}  // namespace wa::log_region_prefs
