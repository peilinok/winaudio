#include <gtest/gtest.h>
#include "FrameProbe.h"
#include "imgui.h"
#include <windows.h>

TEST(FrameProbeUi, TabClickCompletesOnlyWhenTargetContentsBecomeVisible) {
    struct Context {
        Context() { ImGui::CreateContext(); }
        ~Context() { ImGui::DestroyContext(); }
    } context;
    auto& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 800);
    io.DeltaTime = 1.f / 60.f;
    io.IniFilename = nullptr;
    ASSERT_TRUE(io.ConfigInputTrickleEventQueue); // Match production input handling.
    unsigned char* pixels = nullptr;
    int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    static uint64_t tick = 100;
    tick = 100;
    wa::FrameProbe probe([] { return tick; }, 1000);
    probe.setEnabled(true);
    ImVec2 renderCenter;
    bool renderVisible = false;
    const auto frame = [&] {
        probe.beginFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(1280, 800));
        ImGui::Begin("probe");
        if (ImGui::BeginTabBar("pages")) {
            bool visible = ImGui::BeginTabItem("Monitor");
            probe.pageState("Monitor", ImGui::IsItemClicked(), visible);
            if (visible) ImGui::EndTabItem();
            visible = ImGui::BeginTabItem("Render");
            renderVisible = visible;
            const auto min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
            renderCenter = ImVec2((min.x + max.x) / 2, (min.y + max.y) / 2);
            probe.pageState("Render", ImGui::IsItemClicked(), visible);
            if (visible) ImGui::EndTabItem();
            ImGui::EndTabBar();
        }
        ImGui::End();
        ImGui::Render();
        tick += 5;
        probe.endFrame();
    };
    frame();
    tick = 120;
    frame();
    ASSERT_FALSE(renderVisible);
    // Tabs allow overlapping items and require a preceding hovered frame.
    tick = 130;
    io.AddMousePosEvent(renderCenter.x, renderCenter.y);
    frame();
    tick = 140;
    probe.noteInput(WM_LBUTTONDOWN, 30);
    io.AddMouseButtonEvent(0, true);
    frame();
    EXPECT_FALSE(renderVisible); // Vendored ImGui defers the tab selection.
    EXPECT_EQ(probe.operationCount(), 0u);
    io.AddMouseButtonEvent(0, false);
    tick = 180; // Includes the intervening frame/present wait.
    frame();
    ASSERT_TRUE(renderVisible);
    ASSERT_EQ(probe.operationCount(), 1u);
    EXPECT_STREQ(probe.operation(0).kind, "Render");
    EXPECT_EQ(probe.operation(0).start, 110u);
    EXPECT_EQ(probe.operation(0).end, 185u);
    EXPECT_EQ(probe.operation(0).frame, 5u);
}
