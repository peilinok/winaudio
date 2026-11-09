#pragma once
#include "CallLogFilter.h"
#include <string>

namespace wa::log_view {

// Pump messages start with HH:MM:SS.mmm [level]. Other UI events are unclassified.
inline int severity(const std::string& line) {
    if (line.size() < 17 || line[2] != ':' || line[5] != ':' || line[8] != '.' ||
        line[12] != ' ' || line[13] != '[' || line[15] != ']') return -1;
    const std::string levels = "TDIWE";
    const auto index = levels.find(line[14]);
    return index == std::string::npos ? -1 : static_cast<int>(index);
}

inline bool matches(const std::string& line, const char* search, int displayLevel) {
    const int level = severity(line);
    return call_log_filter::fuzzyMatch(line, search) &&
        (level < 0 || displayLevel == 0 || level >= displayLevel + 1);
}

} // namespace wa::log_view
