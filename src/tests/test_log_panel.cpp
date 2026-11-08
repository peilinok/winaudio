#include <gtest/gtest.h>
#include "LogPanel.h"
#include "CallLogFilter.h"
#include <algorithm>
#include <string>
#include <vector>

namespace {
class LogPanel : public testing::Test {
protected:
    wa::log_panel::View view;
    std::vector<std::string> lines;
    uint64_t firstLine = 0;
    bool alwaysScrollbar = true;
    ImFont* font = nullptr;
    struct Frame {
        wa::log_panel::Work work;
        float end = 0, scroll = 0, scrollMax = 0;
        std::vector<ImDrawVert> vertices;
    };

    void SetUp() override {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(800, 600);
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        io.Fonts->SetTexID(reinterpret_cast<ImTextureID>(1));
    }
    void TearDown() override { ImGui::DestroyContext(); }

    Frame render(const char* filter = "", bool reference = false, float width = 380,
                 float scroll = -1, float scale = 1, bool follow = false) {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(600, 400));
        ImGui::Begin("Log", nullptr, ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar);
        ImGui::BeginChild("Rows", ImVec2(width, 180), true,
            alwaysScrollbar ? ImGuiWindowFlags_AlwaysVerticalScrollbar : ImGuiWindowFlags_None);
        if (font) ImGui::PushFont(font);
        ImGui::SetWindowFontScale(scale);
        if (scroll >= 0) ImGui::SetScrollY(scroll);
        const auto matches = [&](const std::string& line) {
            return wa::call_log_filter::fuzzyMatch(line, filter);
        };
        Frame result;
        if (reference) {
            ImGui::PushTextWrapPos(0.f);
            for (const auto& line : lines)
                if (matches(line)) ImGui::TextUnformatted(line.c_str());
            ImGui::PopTextWrapPos();
        } else {
            result.work = view.draw(lines, firstLine, filter, matches);
        }
        if (follow) ImGui::SetScrollHereY(1.f);
        result.end = ImGui::GetCursorPosY();
        result.scroll = ImGui::GetScrollY();
        result.scrollMax = ImGui::GetScrollMaxY();
        const auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
        result.vertices.assign(vertices.begin(), vertices.end());
        if (font) ImGui::PopFont();
        ImGui::EndChild();
        ImGui::End();
        ImGui::Render();
        return result;
    }
    wa::log_panel::Work frame(const char* filter = "") { return render(filter).work; }

    void expectReference(const char* filter = "", float width = 380, float scroll = 0,
                         float scale = 1, bool follow = false) {
        for (int i = 0; i < 3; ++i) render(filter, true, width, scroll, scale, follow);
        const auto expected = render(filter, true, width, scroll, scale, follow);
        for (int i = 0; i < 3; ++i) render(filter, false, width, scroll, scale, follow);
        const auto actual = render(filter, false, width, scroll, scale, follow);
        EXPECT_NEAR(actual.end, expected.end, .05f);
        EXPECT_NEAR(actual.scroll, expected.scroll, .05f);
        EXPECT_NEAR(actual.scrollMax, expected.scrollMax, .05f);
        ASSERT_EQ(actual.vertices.size(), expected.vertices.size());
        for (size_t i = 0; i < actual.vertices.size(); ++i) {
            EXPECT_NEAR(actual.vertices[i].pos.x, expected.vertices[i].pos.x, .05f) << i;
            EXPECT_NEAR(actual.vertices[i].pos.y, expected.vertices[i].pos.y, .05f) << i;
            EXPECT_FLOAT_EQ(actual.vertices[i].uv.x, expected.vertices[i].uv.x) << i;
            EXPECT_FLOAT_EQ(actual.vertices[i].uv.y, expected.vertices[i].uv.y) << i;
            EXPECT_EQ(actual.vertices[i].col, expected.vertices[i].col) << i;
        }
    }
    void mixedHistory() {
        const std::string samples[] = {
            "", "short", "embedded\nline\nbreak", "trailing newline\n", "## visible label",
            std::string(240, 'W'), "UTF-8: \xe4\xb8\xad\xe6\x96\x87 \xe6\x97\xa5\xe5\xbf\x97",
            "capture frames=480 flags=0 ret=S_OK with words that wrap at different widths"
        };
        for (int i = 0; i < 160; ++i)
            lines.push_back(samples[i % 8]);
    }
};
}

TEST_F(LogPanel, StaticHistoryDoesNotRepeatFilteringOrSubmitOffscreenRows) {
    for (int i = 0; i < 5000; ++i)
        lines.push_back("23:00:00.000 [I] capture frames=480 flags=0 ret=S_OK " + std::to_string(i));
    frame();
    const auto work = frame();
    EXPECT_EQ(work.checked, 0u);
    EXPECT_EQ(work.measured, 0u);
    EXPECT_LT(work.submitted, 30u);
}

TEST_F(LogPanel, UnmatchedStaticHistoryDoesNotRepeatSearch) {
    lines.assign(5000, "capture frames=480 ret=S_OK");
    frame("notpresent");
    const auto work = frame("notpresent");
    EXPECT_EQ(work.checked, 0u);
    EXPECT_EQ(work.submitted, 0u);
}

TEST_F(LogPanel, AppendChecksAndMeasuresOnlyNewLines) {
    lines.assign(5000, "capture frames=480 ret=S_OK");
    frame();
    lines.push_back("new capture line");
    const auto work = frame();
    EXPECT_EQ(work.checked, 1u);
    EXPECT_EQ(work.measured, 1u);
    EXPECT_LT(work.submitted, 30u);
}

TEST_F(LogPanel, TrimAndAppendAtCapacityKeepIncrementalWorkAndCorrectRows) {
    mixedHistory();
    frame("capture");
    for (int i = 0; i < 40; ++i) {
        lines.erase(lines.begin());
        ++firstLine;
        lines.push_back(i % 2 ? "new capture" : "other event");
        const auto work = frame("capture");
        EXPECT_EQ(work.checked, 1u);
        EXPECT_EQ(work.measured, i % 2 ? 1u : 0u);
    }
    expectReference("capture", 380, 240);
}

TEST_F(LogPanel, ClearAndRefillWithSameCountCannotReuseOldMatches) {
    lines.assign(100, "capture");
    frame("capture");
    firstLine += lines.size();
    lines.assign(100, "render");
    const auto work = frame("capture");
    EXPECT_EQ(work.checked, 100u);
    EXPECT_EQ(work.submitted, 0u);
    expectReference("render");
}

TEST_F(LogPanel, EntireUnseenReplacementAndCounterResetRebuildCorrectly) {
    mixedHistory();
    frame("capture");
    firstLine += 10000;
    lines.assign(80, "new render line");
    EXPECT_EQ(frame("capture").submitted, 0u);
    expectReference("render");
    firstLine = 0;
    lines.assign(80, "capture after reset");
    expectReference("capture");
}

TEST_F(LogPanel, WrappedEmptyMultilineAndUtf8RowsMatchFullSubmissionAtEveryScroll) {
    mixedHistory();
    for (float scroll : {0.f, 73.f, 1200.f, 100000.f})
        expectReference("", 380, scroll);
}

TEST_F(LogPanel, ResizeAndFontScaleChangesRebuildWrapHeights) {
    mixedHistory();
    for (float width : {380.f, 190.f, 510.f})
        for (float scale : {1.f, 1.25f, 1.5f})
            expectReference("", width, 300.f, scale);
}

TEST_F(LogPanel, FilterChangesPreserveFullResultAndClampOldScroll) {
    mixedHistory();
    expectReference("", 380, 100000.f);
    expectReference("capture", 380, 100000.f);
    expectReference("notpresent", 380, 100000.f);
    expectReference("", 380, 100000.f);
}

TEST_F(LogPanel, FollowBottomAfterAppendTrimAndClearMatchesFullSubmission) {
    mixedHistory();
    expectReference("", 380, -1, 1, true);
    lines.push_back("new capture\nwith extra line");
    expectReference("", 380, -1, 1, true);
    firstLine += 20;
    lines.erase(lines.begin(), lines.begin() + 20);
    expectReference("", 380, -1, 1, true);
    firstLine += lines.size();
    lines.clear();
    expectReference("", 380, -1, 1, true);
}

TEST_F(LogPanel, RowTallerThanViewportKeepsItsPartiallyVisibleText) {
    lines = {std::string(6000, 'W'), "last"};
    for (float scroll : {0.f, 70.f, 320.f, 100000.f})
        expectReference("", 190, scroll);
}

TEST_F(LogPanel, DrawingAndFilteringNeverModifyStoredLogContent) {
    mixedHistory();
    const auto original = lines;
    expectReference("capture", 190, 500);
    expectReference("", 380, 200);
    EXPECT_EQ(lines, original);
}

TEST_F(LogPanel, AutomaticScrollbarAppearingAndDisappearingKeepsWrapLayout) {
    alwaysScrollbar = false;
    mixedHistory();
    expectReference();
    expectReference("notpresent");
    expectReference("capture");
    firstLine += lines.size();
    lines = {"single short line"};
    expectReference();
    firstLine += lines.size();
    lines.clear();
    expectReference();
}

TEST_F(LogPanel, FontReplacementAtTheSameSizeInvalidatesCachedWrapHeights) {
    mixedHistory();
    frame();
    ImFontConfig config;
    config.GlyphExtraSpacing.x = 2.f;
    font = ImGui::GetIO().Fonts->AddFontDefault(&config);
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    EXPECT_EQ(frame().measured, lines.size());
    expectReference("", 380, 300);
}

TEST_F(LogPanel, FractionalItemSpacingMatchesFullSubmissionWhileScrolled) {
    mixedHistory();
    for (float spacing : {0.f, .5f, 4.3f, 4.5f}) {
        SCOPED_TRACE(::testing::Message() << "spacing=" << spacing);
        ImGui::GetStyle().ItemSpacing.y = spacing;
        expectReference("", 380, 300, 1.25f);
        expectReference("", 190, 100000, 1.5f);
    }
}
