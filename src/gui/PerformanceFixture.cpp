#include "PerformanceFixture.h"
#include "AppUi.h"
#include "Log.h"
#include "GuiGpu.h"
#include <d3d11.h>
#include "imgui.h"
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>

namespace {
std::chrono::steady_clock::time_point started;
int tracks = 2;
double warmup = 30, duration = 120;
int phase = 0;
bool warming = true;
std::array<std::array<float, 2>, 4> positions{};
const char* stages[] = {"track1-wave", "track1-spec", "track2-wave", "track2-spec"};
}

bool PerformanceFixture::start(AppUi& ui) {
    if (const char* value = std::getenv("WINAUDIO_PERF_TRACKS")) tracks = std::atoi(value);
    if (const char* value = std::getenv("WINAUDIO_PERF_WARMUP")) warmup = std::atof(value);
    if (const char* value = std::getenv("WINAUDIO_PERF_DURATION")) duration = std::atof(value);
    if ((tracks != 1 && tracks != 2 && tracks != 4) || warmup < 0 || duration <= 0) return false;
    ui.logCollapsed_ = false;
    wa::RenderTrackCreate recipe;
    const wchar_t* endpoint = _wgetenv(L"WINAUDIO_PERF_ENDPOINT");
    if (!endpoint || !*endpoint) return false;
    recipe.deviceId = endpoint;
    recipe.source = {48000, 8, 32, true, 32, 0x63f};
    recipe.mode = wa::RenderLayoutMode::SystemDefault;
    for (int i = 0; i < tracks; ++i) {
        wa::TrackId id = 0;
        const auto result = ui.renderTracks_.create(recipe, &id);
        if (!result) {
            WA_LOG(wa::log::Level::Err, "PerformanceFixture", "create", "RenderTrack", result.message);
            return false;
        }
        for (uint16_t channel = 0; channel < 8; ++channel) {
            if (!ui.renderTracks_.setVolume(id, channel, i % 2 == 0 ? 65 : 25) ||
                !ui.renderTracks_.continueChannel(id, channel)) return false;
        }
    }
    for (const auto& status : ui.renderTracks_.poll()) {
        if (status.state != wa::StreamState::Running || status.chartChannels != 8) return false;
        WA_LOG(wa::log::Level::Info, "PerformanceFixture", "track", std::to_string(status.id),
               std::to_string(status.actualFormat.sampleRate) + "Hz channels=" +
               std::to_string(status.chartChannels));
    }
    RECT client{};
    const HWND hwnd = static_cast<HWND>(ui.pipelineHwnd_);
    const BOOL haveClient = GetClientRect(hwnd, &client);
    const UINT dpi = GetDpiForWindow(hwnd);
    WA_LOG(wa::log::Level::Info, "PerformanceFixture", "GetClientRect/GetDpiForWindow",
           std::to_string(haveClient), std::to_string(client.right - client.left) + "x" +
           std::to_string(client.bottom - client.top) + " dpi=" + std::to_string(dpi));
    IDXGIDevice* dxgi = nullptr;
    HRESULT hr = wa::guiDevice()->QueryInterface(IID_PPV_ARGS(&dxgi));
    WA_LOG(wa::log::Level::Debug, "PerformanceFixture", "QueryInterface", "IDXGIDevice", wa::log::hrName(hr));
    if (SUCCEEDED(hr)) {
        IDXGIAdapter* adapter = nullptr;
        hr = dxgi->GetAdapter(&adapter);
        WA_LOG(wa::log::Level::Debug, "PerformanceFixture", "GetAdapter", "DXGI", wa::log::hrName(hr));
        if (SUCCEEDED(hr)) {
            DXGI_ADAPTER_DESC desc{};
            hr = adapter->GetDesc(&desc);
            char name[256]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
            WA_LOG(wa::log::Level::Info, "PerformanceFixture", "GetDesc", wa::log::hrName(hr), name);
            adapter->Release();
        }
        dxgi->Release();
    }
    started = std::chrono::steady_clock::now();
    return true;
}

bool PerformanceFixture::advance(AppUi& ui) {
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (elapsed >= warmup + duration) return false;
    const int count = tracks == 1 ? 2 : 4;
    phase = elapsed < warmup ? 0 : static_cast<int>((elapsed - warmup) / (duration / count));
    if (phase >= count) phase = count - 1;
    warming = elapsed < warmup;
    ui.frameProbe_.setStage(warming ? "warmup" : stages[phase]);
    return true;
}

void PerformanceFixture::scrollCharts(AppUi& ui) {
    const float target = positions[static_cast<size_t>(phase / 2)][static_cast<size_t>(phase % 2)];
    if (!warming && std::abs(ImGui::GetScrollY() - target) > 1.f)
        ui.frameProbe_.setStage("transition");
    ImGui::SetScrollY(target);
}

void PerformanceFixture::chartPosition(uint64_t source, bool spectrum) {
    if (source == 0 || source > positions.size()) return;
    positions[static_cast<size_t>(source - 1)][spectrum ? 1u : 0u] = ImGui::GetCursorPosY();
}
