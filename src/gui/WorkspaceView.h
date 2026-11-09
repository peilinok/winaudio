#pragma once
#include <algorithm>
#include <cstdint>
#include <vector>

namespace wa::workspace_view {

struct Tracks {
    uint64_t selected = 0;
    uint64_t compared = 0;
    int mode = 0; // 0 = selected, 1 = compare, 2 = all

    void reconcile(const std::vector<uint64_t>& live) {
        const auto contains = [&](uint64_t id) {
            return id && std::find(live.begin(), live.end(), id) != live.end();
        };
        if (!contains(selected)) selected = live.empty() ? 0 : live.front();
        if (!contains(compared) || compared == selected) compared = 0;
    }

    void select(uint64_t id) {
        if (compared == id) compared = selected;
        selected = id;
    }

    bool visible(uint64_t id) const {
        return mode == 2 || id == selected || (mode == 1 && id == compared);
    }
};

} // namespace wa::workspace_view
