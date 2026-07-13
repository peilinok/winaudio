#pragma once
#include <cstdio>
#include <string>
#include <utility>
#include <vector>
#include "AudioSessionEnumerator.h"

namespace wa::app_loopback_ui {

inline bool copySessionPidToBuffer(const std::vector<AudioSessionProcess>& rows,
                                   int index, char* dst, size_t dstSize) {
    if (!dst || dstSize == 0 || index < 0 || index >= static_cast<int>(rows.size()))
        return false;
    std::snprintf(dst, dstSize, "%u", rows[static_cast<size_t>(index)].processId);
    return true;
}

inline void applyRefreshResult(bool ok, std::vector<AudioSessionProcess>& target,
                               bool& loaded, int& selectedIndex,
                               std::vector<AudioSessionProcess>&& rows) {
    loaded = true;
    if (!ok) {
        target.clear();
        selectedIndex = -1;
        return;
    }
    target = std::move(rows);
    if (selectedIndex >= static_cast<int>(target.size()))
        selectedIndex = -1;
}

} // namespace wa::app_loopback_ui
