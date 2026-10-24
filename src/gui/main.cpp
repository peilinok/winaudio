// Dear ImGui: standalone example application for DirectX 11
// Adapted for WinAudio: runs the AppUi frame loop (monitor-only, driven by wa::MonitorEngine).

// Learn about Dear ImGui:
// - FAQ                  https://dearimgui.com/faq
// - Getting Started      https://dearimgui.com/getting-started
// - Documentation        https://dearimgui.com/docs (same as your local docs/ folder).
// - Introduction, links and more at the top of imgui.cpp

#include "AppUi.h"
#ifdef WA_PERFORMANCE_FIXTURE
#include "PerformanceFixture.h"
#endif
#include "GuiGpu.h"
#include "Log.h"
#include "imgui.h"
#include "implot.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <d3d11.h>
#include <tchar.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

// Data
static ID3D11Device*            g_pd3dDevice = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext = nullptr;
static IDXGISwapChain*          g_pSwapChain = nullptr;
static bool                     g_SwapChainOccluded = false;
static UINT                     g_ResizeWidth = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView = nullptr;

// Forward declarations of helper functions
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static bool writeFrameProbeRecords(FILE* file, const wa::FrameProbe& probe) {
    char line[1536];
    bool ok = true;
    const auto emit = [&](int n) {
        if (n > 0 && std::fwrite(line, 1, static_cast<size_t>(n), file) != static_cast<size_t>(n))
            ok = false;
    };
    for (int i = 0; i < static_cast<int>(wa::FrameSlice::Count); ++i) {
        if (i == static_cast<int>(wa::FrameSlice::ProbeIo)) continue;
        emit(probe.formatLine(static_cast<wa::FrameSlice>(i), line, sizeof(line)));
    }
    for (size_t i = 0; i < probe.operationCount(); ++i)
        emit(probe.formatOperation(i, line, sizeof(line)));
    for (size_t i = 0; i < probe.workCount(); ++i)
        emit(probe.formatWork(i, line, sizeof(line)));
    return ok;
}

// Unset, empty, and "0" leave the probe off. Read once, before the frame loop.
static bool frameProbeRequested() {
    const char* value = std::getenv("WINAUDIO_FRAME_PROBE");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
}

// Main code
int main(int, char**)
{
    // Create application window
    //ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr, L"ImGui Example", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"WinAudio", WS_OVERLAPPEDWINDOW, 100, 100, 1280, 800, nullptr, nullptr, wc.hInstance, nullptr);

    // Initialize Direct3D
    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // Show the window
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();
    //ImGui::StyleColorsLight();

    // Setup Platform/Renderer backends
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
    wa::setGuiGpu(g_pd3dDevice, g_pd3dDeviceContext);

    // Load Fonts
    // - If no fonts are loaded, dear imgui will use the default font. You can also load multiple fonts and use ImGui::PushFont()/PopFont() to select them.
    // - AddFontFromFileTTF() will return the ImFont* so you can store it if you need to select the font among multiple.
    // - If the file cannot be loaded, the function will return a nullptr. Please handle those errors in your application (e.g. use an assertion, or display an error and quit).
    // - The fonts will be rasterized at a given size (w/ oversampling) and stored into a texture when calling ImFontAtlas::Build()/GetTexDataAsXXXX(), which ImGui_ImplXXXX_NewFrame below will call.
    // - Use '#define IMGUI_ENABLE_FREETYPE' in your imconfig file to use Freetype for higher quality font rendering.
    // - Read 'docs/FONTS.md' for more instructions and details.
    // - Remember that in C/C++ if you want to include a backslash \ in a string literal you need to write a double backslash \\ !
    //io.Fonts->AddFontDefault();
    //io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\segoeui.ttf", 18.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/DroidSans.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Roboto-Medium.ttf", 16.0f);
    //io.Fonts->AddFontFromFileTTF("../../misc/fonts/Cousine-Regular.ttf", 15.0f);
    //ImFont* font = io.Fonts->AddFontFromFileTTF("c:\\Windows\\Fonts\\ArialUni.ttf", 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
    //IM_ASSERT(font != nullptr);

    // Our state
    ImVec4 clear_color = ImVec4(0.45f, 0.55f, 0.60f, 1.00f);

    // WinAudio: UI (owns both engine and monitor engine)
    static AppUi ui;
    ui.setMainWindow(hwnd);

    // WinAudio: logging — file (winaudio.log, exe dir) + GUI panel via callback sink.
    // The callback runs on the logging pump thread; pushLog buffers thread-safely.
    // Init before the font load so a missing face can be recorded.
    wa::log::init();
    wa::log::setThreadName("main");
    wa::log::addFileSink("winaudio.log");
    wa::log::addCallbackSink([](wa::log::Level lvl, const std::string& line) {
        ui.pushLog(static_cast<int>(lvl), line);
    });
    wa::log::setLevel(wa::log::Level::Info);

    // System Microsoft YaHei, once, for both GUI languages. Not redistributed.
    // Skip AddFontFromFileTTF when the file is absent: that call asserts in
    // Debug before it can return null, so the fallback would never run.
    constexpr const char* kYaHeiPath = "C:\\Windows\\Fonts\\msyh.ttc";
    std::error_code yaHeiEc;
    const bool yaHeiPresent = std::filesystem::exists(kYaHeiPath, yaHeiEc) && !yaHeiEc;
    if (!yaHeiPresent ||
        !io.Fonts->AddFontFromFileTTF(kYaHeiPath, 18.0f, nullptr,
                                     io.Fonts->GetGlyphRangesChineseSimplifiedCommon())) {
        io.Fonts->AddFontDefault();
        WA_LOG(wa::log::Level::Warn, "GuiFont", "AddFontFromFileTTF", kYaHeiPath,
               yaHeiPresent ? "failed" : "missing");
    }
    ui.loadLogRegionPrefs();
    ui.loadProductionPhrases();
#ifdef WA_PERFORMANCE_FIXTURE
    const bool fixtureReady = PerformanceFixture::start(ui);
#endif

    // Append records after Present. Failure leaves the probe off. Not wa::log:
    // a log line would be counted by the next frame's log-drain slice.
    FILE* frameProbeLog = nullptr;
    const wchar_t* probePath = _wgetenv(L"WINAUDIO_FRAME_PROBE_PATH");
    const char* probeFlush = std::getenv("WINAUDIO_FRAME_PROBE_FLUSH");
    const bool flushEveryFrame = probeFlush && std::strcmp(probeFlush, "1") == 0;
    if (frameProbeRequested() &&
        _wfopen_s(&frameProbeLog, probePath && *probePath ? probePath : L"winaudio-frame-probe.log",
                  L"a") == 0 && frameProbeLog) {
        std::setvbuf(frameProbeLog, nullptr, _IOFBF, 1024 * 1024);
        ui.frameProbe().setEnabled(true);
        const char* stage = std::getenv("WINAUDIO_FRAME_PROBE_STAGE");
        if (stage && *stage) ui.frameProbe().setStage(stage);
        const char* header = wa::FrameProbe::headerLine();
        std::fwrite(header, 1, std::strlen(header), frameProbeLog);
        std::fflush(frameProbeLog);
    }

    // Main loop
    bool done = false;
#ifdef WA_PERFORMANCE_FIXTURE
    done = !fixtureReady;
#endif
    while (!done)
    {
        // Poll and handle messages (inputs, window resize, etc.)
        // See the WndProc() function below for our to dispatch events to the Win32 backend.
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            if (ui.frameProbe().enabled() &&
                (msg.message == WM_LBUTTONDOWN || msg.message == WM_LBUTTONDBLCLK))
                ui.frameProbe().noteInput(msg.message, ::GetTickCount() - msg.time);
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        // Handle window being minimized or screen locked
        if (g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // Handle window resize (we don't resize directly in the WM_SIZE handler)
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
        }

        // Start the Dear ImGui frame
#ifdef WA_PERFORMANCE_FIXTURE
        if (!PerformanceFixture::advance(ui)) break;
#endif
        ui.frameProbe().beginFrame();
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // WinAudio: draw the application UI
        {
            wa::FrameBracket uiBracket(ui.frameProbe(), wa::FrameSlice::Ui);
            ui.draw();
        }

        // Rendering
        ImGui::Render();
        const float clear_color_with_alpha[4] = { clear_color.x * clear_color.w, clear_color.y * clear_color.w, clear_color.z * clear_color.w, clear_color.w };
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, nullptr);
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);
        {
            wa::FrameBracket drawBracket(ui.frameProbe(), wa::FrameSlice::DrawSubmit);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        }
        ui.frameProbe().endFrame();

        // Present(1, 0) only. The occluded Present(0, DXGI_PRESENT_TEST) above is not this slice.
        HRESULT hr = S_OK;
        {
            wa::FrameBracket presentBracket(ui.frameProbe(), wa::FrameSlice::Present);
            hr = g_pSwapChain->Present(1, 0);   // Present with vsync
        }
        //HRESULT hr = g_pSwapChain->Present(0, 0); // Present without vsync
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
        if (frameProbeLog) {
            auto& probe = ui.frameProbe();
            probe.begin(wa::FrameSlice::ProbeIo);
            bool ok = writeFrameProbeRecords(frameProbeLog, probe);
            if (flushEveryFrame && std::fflush(frameProbeLog) != 0) ok = false;
            probe.end(wa::FrameSlice::ProbeIo);
            // The timing trailer cannot include its own write; the full frame interval does.
            char trailer[1536];
            int n = probe.formatLine(wa::FrameSlice::ProbeIo, trailer, sizeof(trailer));
            if (n > 0 && std::fwrite(trailer, 1, static_cast<size_t>(n), frameProbeLog) != static_cast<size_t>(n)) ok = false;
            n = probe.formatFrame(trailer, sizeof(trailer));
            if (n > 0 && std::fwrite(trailer, 1, static_cast<size_t>(n), frameProbeLog) != static_cast<size_t>(n)) ok = false;
            if (!ok || std::ferror(frameProbeLog)) {
                WA_LOG(wa::log::Level::Warn, "FrameProbe", "write", "records", "failed; measurement incomplete");
                probe.setEnabled(false);
                std::fclose(frameProbeLog);
                frameProbeLog = nullptr;
            }
        }
    }

    // Keep shutdown resource work separate from rendered workload frames.
    ui.frameProbe().setStage("teardown");
    ui.frameProbe().beginFrame();
    ui.stopAll();
    ui.releaseGpu();  // previous draw consumed its SRVs; device is still alive
    ui.frameProbe().endFrame();
    if (frameProbeLog) {
        bool ok = writeFrameProbeRecords(frameProbeLog, ui.frameProbe());
        char metadata[1536];
        const int n = ui.frameProbe().formatFrame(metadata, sizeof(metadata));
        if (n <= 0 || std::fwrite(metadata, 1, static_cast<size_t>(n), frameProbeLog) != static_cast<size_t>(n))
            ok = false;
        std::fprintf(frameProbeLog, "# End frame probe frame=%llu complete=%d\n",
                     static_cast<unsigned long long>(ui.frameProbe().frameNumber()), ok ? 1 : 0);
        if (std::fclose(frameProbeLog) != 0 || !ok)
            WA_LOG(wa::log::Level::Warn, "FrameProbe", "close", "records", "failed; measurement incomplete");
        frameProbeLog = nullptr;
    }

    wa::log::shutdown();   // flush + stop the logging pump before teardown

    // Cleanup
    wa::setGuiGpu(nullptr, nullptr);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);

#ifdef WA_PERFORMANCE_FIXTURE
    return fixtureReady ? 0 : 2;
#else
    return 0;
#endif
}

// Helper functions

bool CreateDeviceD3D(HWND hWnd)
{
    // Setup swap chain
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    //createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED) // Try high-performance WARP software driver if hardware is not available.
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

void CreateRenderTarget()
{
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Win32 message handler
// You can read the io.WantCaptureMouse, io.WantCaptureKeyboard flags to tell if dear imgui wants to use your inputs.
// - When io.WantCaptureMouse is true, do not dispatch mouse input data to your main application, or clear/overwrite your copy of the mouse data.
// - When io.WantCaptureKeyboard is true, do not dispatch keyboard input data to your main application, or clear/overwrite your copy of the keyboard data.
// Generally you may always pass all inputs to dear imgui, and hide them from your application based on those two flags.
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    if (msg == wa::liveSessionDirtyMessage() || msg == wa::liveSessionCellMessage())
        return 0;

    switch (msg)
    {
    case WM_CLOSE:
        ::PostQuitMessage(0);
        return 0;
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_ResizeWidth = (UINT)LOWORD(lParam); // Queue resize
        g_ResizeHeight = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}
