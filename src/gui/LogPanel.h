#pragma once
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace wa::log_panel {

struct Work {
    size_t checked = 0;
    size_t measured = 0;
    size_t submitted = 0;
};

class View {
public:
    // Source entries are immutable. firstLine advances by the number removed
    // on a front trim or clear, so replacement at the same size stays distinct.
    // filterKey identifies every input used by the matching predicate.
    template<class Matches>
    Work draw(const std::vector<std::string>& lines, uint64_t firstLine,
              std::string_view filterKey, Matches matches) {
        Work work;
        const Layout layout{ImGui::GetFont(), ImGui::GetFontSize(),
            (std::max)(1.f, ImGui::GetContentRegionAvail().x), ImGui::GetStyle().ItemSpacing.y};
        const uint64_t end = firstLine + lines.size();
        if (!initialized_ || key_ != filterKey || !(layout == layout_) ||
            firstLine < firstLine_ || end < end_ || firstLine > end_) {
            rows_.clear();
            base_ = negativeBase_ = 0;
            end_ = firstLine;
            key_ = filterKey;
            layout_ = layout;
            initialized_ = true;
        }
        while (!rows_.empty() && rows_.front().id < firstLine) {
            base_ = rows_.front().bottom;
            negativeBase_ = rows_.front().negativeBottom;
            rows_.pop_front();
        }
        for (uint64_t id = end_; id < end; ++id) {
            const auto& line = lines[static_cast<size_t>(id - firstLine)];
            ++work.checked;
            if (matches(line)) {
                const float advance = ImGui::CalcTextSize(
                    line.c_str(), nullptr, false, layout.width).y + layout.spacing;
                const float height = std::floor(advance), negativeHeight = std::ceil(advance);
                const double top = rows_.empty() ? base_ : rows_.back().bottom;
                const double negativeTop = rows_.empty() ? negativeBase_ : rows_.back().negativeBottom;
                rows_.push_back({id, top + height, negativeTop + negativeHeight, height, negativeHeight});
                ++work.measured;
            }
        }
        firstLine_ = firstLine;
        end_ = end;
        if (rows_.empty()) return work;

        ImGui::PushTextWrapPos(0.f);
        // Let ImGui place the first row, including its initial text baseline.
        // Every following cursor position is aligned to an integer pixel.
        ImGui::TextUnformatted(lines[static_cast<size_t>(rows_.front().id - firstLine)].c_str());
        ++work.submitted;
        if (rows_.size() == 1) {
            ImGui::PopTextWrapPos();
            return work;
        }
        const float origin = ImGui::GetCursorScreenPos().y;
        const auto begin = rows_.begin() + 1;
        const double base = rows_.front().bottom, negativeBase = rows_.front().negativeBottom;
        // ItemSize truncates toward zero: fractional advances round up above
        // the screen origin and down below it. Keep both prefix sums so font
        // scaling and scrolling reproduce full submission without rescanning.
        const auto positive = std::upper_bound(begin, rows_.end(), negativeBase - origin,
            [](double height, const Row& row) { return height < row.negativeBottom; });
        const double correction = (positive - 1)->negativeBottom - negativeBase -
            ((positive - 1)->bottom - base);
        const uint64_t positiveId = positive == rows_.end() ? end_ : positive->id;
        const auto bottom = [&](const Row& row) {
            return row.id < positiveId ? row.negativeBottom - negativeBase : row.bottom - base + correction;
        };
        const auto* drawList = ImGui::GetWindowDrawList();
        if (drawList->GetClipRectMax().y <= drawList->GetClipRectMin().y) {
            skip(bottom(rows_.back()), layout.spacing);
            ImGui::PopTextWrapPos();
            return work;
        }
        const double clipTop = drawList->GetClipRectMin().y - origin;
        const double clipBottom = drawList->GetClipRectMax().y - origin;
        const auto after = [&](double height, const Row& row) { return height < bottom(row); };
        auto first = std::upper_bound(begin, rows_.end(), clipTop, after);
        auto last = std::upper_bound(begin, rows_.end(), clipBottom, after);
        // One neighboring row on each side keeps partially clipped text and
        // item-spacing boundaries consistent with full ImGui submission.
        if (first != begin) --first;
        if (last != rows_.end()) ++last;
        const double prefix = bottom(*first) - (first->id < positiveId ? first->negativeHeight : first->height);
        if (prefix > 0)
            skip(prefix, layout.spacing);
        for (auto row = first; row != last; ++row) {
            ImGui::TextUnformatted(lines[static_cast<size_t>(row->id - firstLine)].c_str());
            ++work.submitted;
        }
        ImGui::PopTextWrapPos();
        const double suffix = bottom(rows_.back()) - bottom(*(last - 1));
        if (suffix > 0)
            skip(suffix, layout.spacing);
        return work;
    }

private:
    static void skip(double height, float spacing) {
        auto cursor = ImGui::GetCursorScreenPos();
        // Add the integer advance before subtracting fractional spacing;
        // a tall dummy's opposite operation order can lose a pixel at zero.
        cursor.y = (cursor.y + static_cast<float>(height)) - spacing;
        ImGui::SetCursorScreenPos(cursor);
        ImGui::Dummy(ImVec2(0, 0));
    }
    struct Row { uint64_t id; double bottom, negativeBottom; float height, negativeHeight; };
    struct Layout {
        ImFont* font;
        float fontSize, width, spacing;
        bool operator==(const Layout& other) const {
            return font == other.font && fontSize == other.fontSize &&
                width == other.width && spacing == other.spacing;
        }
    };
    std::deque<Row> rows_;
    std::string key_;
    Layout layout_{};
    double base_ = 0, negativeBase_ = 0;
    uint64_t firstLine_ = 0, end_ = 0;
    bool initialized_ = false;
};

} // namespace wa::log_panel
