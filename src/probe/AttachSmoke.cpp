#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <chrono>
#include "OnDemandAttach.h"
#include "PipelineGraph.h"

static int runVictim() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return 2;
    IMMDeviceEnumerator* enumer = nullptr;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumer));
    if (FAILED(hr) || !enumer) return 3;
    IMMDevice* dev = nullptr;
    hr = enumer->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
    enumer->Release();
    if (FAILED(hr) || !dev) return 4;
    IAudioClient* client = nullptr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&client));
    dev->Release();
    if (FAILED(hr) || !client) return 5;
    WAVEFORMATEX* mix = nullptr;
    hr = client->GetMixFormat(&mix);
    if (FAILED(hr) || !mix) {
        client->Release();
        return 6;
    }
    const UINT32 blockAlign = mix->nBlockAlign ? mix->nBlockAlign : 4;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, nullptr);
    CoTaskMemFree(mix);
    if (FAILED(hr)) {
        client->Release();
        return 7;
    }
    IAudioRenderClient* render = nullptr;
    hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&render));
    if (FAILED(hr) || !render) {
        client->Release();
        return 8;
    }
    UINT32 frames = 0;
    client->GetBufferSize(&frames);
    hr = client->Start();
    if (FAILED(hr)) {
        render->Release();
        client->Release();
        return 9;
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < end) {
        UINT32 pad = 0;
        client->GetCurrentPadding(&pad);
        const UINT32 avail = frames > pad ? frames - pad : 0;
        if (avail > 0) {
            BYTE* data = nullptr;
            if (SUCCEEDED(render->GetBuffer(avail, &data)) && data) {
                std::memset(data, 0, static_cast<size_t>(avail) * blockAlign);
                render->ReleaseBuffer(avail, 0);
            }
        }
        Sleep(5);
    }
    client->Stop();
    render->Release();
    client->Release();
    CoUninitialize();
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--victim") == 0) return runVictim();

    char self[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, self, MAX_PATH);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = std::string("\"") + self + "\" --victim";
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        std::printf("CreateProcess failed %lu\n", GetLastError());
        return 1;
    }
    CloseHandle(pi.hThread);
    Sleep(1500);

    wa::OnDemandAttach attach;
    wa::Result r = attach.start(pi.dwProcessId, {}, wa::PipelineFlow::Render);
    if (!r) {
        std::printf("attach failed: %s\n", r.message.c_str());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        return 10;
    }
    attach.setPumpEnabled(true);
    std::printf("attached pid=%u\n", pi.dwProcessId);

    size_t control = 0;
    size_t pump = 0;
    for (int i = 0; i < 120; ++i) {
        auto more = attach.drain();
        control += more.size();
        auto ring = attach.pumpRing();
        if (ring.size() > pump) pump = ring.size();
        if (pump > 0) break;
        Sleep(50);
    }
    WaitForSingleObject(pi.hProcess, 15000);
    auto more = attach.drain();
    control += more.size();
    auto ring = attach.pumpRing();
    if (ring.size() > pump) pump = ring.size();
    const uint32_t xruns = attach.pumpXruns();
    attach.stop();
    CloseHandle(pi.hProcess);

    std::printf("control_calls=%zu pump_ring=%zu xruns=%u\n", control, pump, xruns);
    if (pump == 0 && control == 0) {
        std::printf("FAIL: empty call log after attach-while-pumping\n");
        return 11;
    }
    std::printf("OK\n");
    return 0;
}