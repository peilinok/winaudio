// Parent process for a manual Application Loopback check.
// Launch this exe. It starts a child whose file name is different, then waits
// until that child exits. The child loops a short prompt tone to the default
// render device and does nothing else. Capture the parent PID with IncludeTree
// and listen for that tone.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "AudioFormat.h"
#include "ComUtil.h"

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kPromptHz = 880.0;
constexpr double kPromptGain = 0.22;
constexpr int kBeepMs = 160;
constexpr int kPeriodMs = 1000;
constexpr wchar_t kChildFileName[] = L"WaLoopbackChildTone.exe";

void outLine(const std::wstring& line) {
    HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (console && GetConsoleMode(console, &mode)) {
        const std::wstring withNl = line + L"\n";
        DWORD written = 0;
        WriteConsoleW(console, withNl.c_str(), static_cast<DWORD>(withNl.size()), &written, nullptr);
        return;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes > 1 ? static_cast<size_t>(bytes - 1) : 0, '\0');
    if (bytes > 1) {
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
    }
    std::fwrite(utf8.data(), 1, utf8.size(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void errLine(const std::wstring& line) {
    HANDLE console = GetStdHandle(STD_ERROR_HANDLE);
    DWORD mode = 0;
    if (console && GetConsoleMode(console, &mode)) {
        const std::wstring withNl = line + L"\n";
        DWORD written = 0;
        WriteConsoleW(console, withNl.c_str(), static_cast<DWORD>(withNl.size()), &written, nullptr);
        return;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(bytes > 1 ? static_cast<size_t>(bytes - 1) : 0, '\0');
    if (bytes > 1) {
        WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, utf8.data(), bytes, nullptr, nullptr);
    }
    std::fwrite(utf8.data(), 1, utf8.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

std::wstring selfPath() {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0 || n >= path.size()) return L"";
    path.resize(n);
    return path;
}

std::wstring fileNameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring quoteArg(const std::wstring& text) {
    std::wstring out = L"\"";
    for (wchar_t ch : text) {
        if (ch == L'"') out += L'\\';
        out += ch;
    }
    out += L'"';
    return out;
}

void printHelp() {
    outLine(L"LoopbackTreeProbe — 给手工 IncludeTree 测试用的父进程");
    outLine(L"");
    outLine(L"用法: LoopbackTreeProbe.exe");
    outLine(L"");
    outLine(L"启动后会拉起一个不同文件名的子进程，然后一直等到该子进程退出。");
    outLine(L"子进程向默认播放设备循环播放一段短提示音，父进程自己不播放。");
    outLine(L"请对父进程 PID 做 Application Loopback（IncludeTree），听采集里有没有这段提示音。");
    outLine(L"关掉子进程窗口，或在子进程窗口按 Ctrl+C，父进程会退出。");
}

struct PromptPlayer {
    std::thread thread;
    std::atomic<bool> running{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool started = false;
    std::string error;
    HANDLE stopEvent = nullptr;

    ~PromptPlayer() { stop(); }

    bool start() {
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stopEvent) {
            error = "CreateEventW failed";
            return false;
        }
        running.store(true);
        try {
            thread = std::thread([this] { threadMain(); });
        } catch (const std::system_error&) {
            running.store(false);
            error = "failed to start render thread";
            return false;
        }
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [this] { return started; });
        return error.empty();
    }

    void requestStop() {
        running.store(false);
        if (stopEvent) SetEvent(stopEvent);
    }

    void stop() {
        requestStop();
        if (thread.joinable()) thread.join();
        if (stopEvent) {
            CloseHandle(stopEvent);
            stopEvent = nullptr;
        }
    }

    void markStarted() {
        std::lock_guard<std::mutex> lock(mutex);
        started = true;
        cv.notify_one();
    }

    void threadMain() {
        wa::ComInitGuard com;
        if (!com.ok()) {
            error = "CoInitializeEx failed";
            markStarted();
            return;
        }

        wa::ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(enumerator.GetAddressOf()));
        if (FAILED(hr)) {
            error = "CoCreateInstance(MMDeviceEnumerator) failed";
            markStarted();
            return;
        }
        wa::ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
        if (FAILED(hr)) {
            error = "GetDefaultAudioEndpoint failed";
            markStarted();
            return;
        }
        wa::ComPtr<IAudioClient> client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) {
            error = "Activate IAudioClient failed";
            markStarted();
            return;
        }
        WAVEFORMATEX* mix = nullptr;
        hr = client->GetMixFormat(&mix);
        if (FAILED(hr) || !mix) {
            error = "GetMixFormat failed";
            markStarted();
            return;
        }
        const wa::AudioFormat format = wa::fromWaveFormat(mix);
        const bool float32 = format.isFloat && format.bitsPerSample == 32;
        const bool pcm16 = !format.isFloat && format.bitsPerSample == 16;
        if ((!float32 && !pcm16) || format.channels == 0 || format.sampleRate == 0) {
            CoTaskMemFree(mix);
            error = "default render mix format is not float32 or pcm16";
            markStarted();
            return;
        }

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                0, 0, mix, nullptr);
        CoTaskMemFree(mix);
        if (FAILED(hr)) {
            error = "IAudioClient::Initialize failed";
            markStarted();
            return;
        }
        UINT32 bufferFrames = 0;
        hr = client->GetBufferSize(&bufferFrames);
        if (FAILED(hr) || bufferFrames == 0) {
            error = "GetBufferSize failed";
            markStarted();
            return;
        }
        HANDLE audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!audioEvent) {
            error = "audio event create failed";
            markStarted();
            return;
        }
        hr = client->SetEventHandle(audioEvent);
        if (FAILED(hr)) {
            CloseHandle(audioEvent);
            error = "SetEventHandle failed";
            markStarted();
            return;
        }
        wa::ComPtr<IAudioRenderClient> render;
        hr = client->GetService(__uuidof(IAudioRenderClient),
                                reinterpret_cast<void**>(render.GetAddressOf()));
        if (FAILED(hr)) {
            CloseHandle(audioEvent);
            error = "GetService IAudioRenderClient failed";
            markStarted();
            return;
        }

        const uint64_t periodSamples =
            static_cast<uint64_t>(format.sampleRate) * static_cast<uint64_t>(kPeriodMs) / 1000u;
        const uint64_t beepSamples =
            static_cast<uint64_t>(format.sampleRate) * static_cast<uint64_t>(kBeepMs) / 1000u;
        const double dPhase = 2.0 * kPi * kPromptHz / static_cast<double>(format.sampleRate);
        uint64_t sampleIndex = 0;
        double phase = 0;

        auto fill = [&](UINT32 frames) -> bool {
            BYTE* data = nullptr;
            if (FAILED(render->GetBuffer(frames, &data)) || !data) return false;
            for (UINT32 i = 0; i < frames; ++i) {
                const uint64_t pos = periodSamples == 0 ? 0 : (sampleIndex % periodSamples);
                const bool sounding = pos < beepSamples;
                if (pos == 0) phase = 0;
                const double raw = sounding ? kPromptGain * std::sin(phase) : 0.0;
                if (sounding) {
                    phase += dPhase;
                    if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
                }
                ++sampleIndex;
                if (float32) {
                    auto* samples = reinterpret_cast<float*>(data);
                    const float sample = static_cast<float>(raw);
                    for (uint16_t ch = 0; ch < format.channels; ++ch) {
                        samples[static_cast<size_t>(i) * format.channels + ch] = sample;
                    }
                } else {
                    auto* samples = reinterpret_cast<int16_t*>(data);
                    long rounded = std::lround(raw * 32767.0);
                    if (rounded > 32767) rounded = 32767;
                    if (rounded < -32767) rounded = -32767;
                    const auto value = static_cast<int16_t>(rounded);
                    for (uint16_t ch = 0; ch < format.channels; ++ch) {
                        samples[static_cast<size_t>(i) * format.channels + ch] = value;
                    }
                }
            }
            return SUCCEEDED(render->ReleaseBuffer(frames, 0));
        };

        if (!fill(bufferFrames)) {
            CloseHandle(audioEvent);
            error = "prefill failed";
            markStarted();
            return;
        }
        hr = client->Start();
        if (FAILED(hr)) {
            CloseHandle(audioEvent);
            error = "IAudioClient::Start failed";
            markStarted();
            return;
        }
        markStarted();

        HANDLE waits[2] = {stopEvent, audioEvent};
        while (running.load()) {
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (!running.load() || wait == WAIT_OBJECT_0) break;
            UINT32 padding = 0;
            if (FAILED(client->GetCurrentPadding(&padding))) break;
            if (padding >= bufferFrames) continue;
            if (!fill(bufferFrames - padding)) break;
        }
        client->Stop();
        CloseHandle(audioEvent);
        if (stopEvent) SetEvent(stopEvent);
    }
};

PromptPlayer* g_player = nullptr;

BOOL WINAPI childCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (g_player) g_player->requestStop();
        return TRUE;
    }
    return FALSE;
}

int childMain() {
    PromptPlayer player;
    g_player = &player;
    SetConsoleCtrlHandler(childCtrl, TRUE);
    SetConsoleTitleW(kChildFileName);
    if (!player.start()) {
        errLine(L"子进程无法向默认设备播放提示音。");
        g_player = nullptr;
        return 2;
    }
    outLine(L"子进程 " + std::wstring(kChildFileName) + L"  pid=" +
            std::to_wstring(GetCurrentProcessId()));
    outLine(L"正在向默认播放设备循环播放提示音。关闭本窗口或按 Ctrl+C 结束。");
    if (player.stopEvent) WaitForSingleObject(player.stopEvent, INFINITE);
    player.stop();
    g_player = nullptr;
    return 0;
}

HANDLE createKillJob() {
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

bool copyChildExe(const std::wstring& self, std::wstring& dest, std::wstring& error) {
    wchar_t temp[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, temp);
    if (n == 0 || n >= MAX_PATH) {
        error = L"GetTempPathW 失败";
        return false;
    }
    const std::wstring dir = std::wstring(temp) + L"winaudio-loopback-tree-probe";
    CreateDirectoryW(dir.c_str(), nullptr);
    dest = dir + L"\\" + kChildFileName;
    if (CompareStringOrdinal(self.c_str(), -1, dest.c_str(), -1, TRUE) == CSTR_EQUAL) {
        error = L"请运行 LoopbackTreeProbe.exe，不要直接运行子进程副本。";
        return false;
    }
    DeleteFileW(dest.c_str());
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (CopyFileW(self.c_str(), dest.c_str(), FALSE)) return true;
        Sleep(200);
    }
    error = L"无法复制子进程文件。如果上次的提示音还在响，先关掉 WaLoopbackChildTone 窗口。";
    return false;
}

int parentMain() {
    const std::wstring self = selfPath();
    if (self.empty()) {
        errLine(L"GetModuleFileNameW 失败");
        return 2;
    }
    std::wstring childExe;
    std::wstring copyError;
    if (!copyChildExe(self, childExe, copyError)) {
        errLine(copyError);
        return 2;
    }

    const std::wstring command = quoteArg(childExe) + L" --child";
    std::vector<wchar_t> buffer(command.begin(), command.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    const DWORD flags = CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP;
    if (!CreateProcessW(childExe.c_str(), buffer.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        nullptr, &startup, &child)) {
        errLine(L"启动子进程失败。");
        return 2;
    }

    HANDLE job = createKillJob();
    if (job) AssignProcessToJobObject(job, child.hProcess);

    outLine(L"父进程");
    outLine(L"  pid=" + std::to_wstring(GetCurrentProcessId()));
    outLine(L"  image=" + self);
    outLine(L"子进程");
    outLine(L"  pid=" + std::to_wstring(child.dwProcessId));
    outLine(L"  image=" + childExe);
    outLine(L"父进程文件名: " + fileNameOf(self));
    outLine(L"子进程文件名: " + fileNameOf(childExe));
    outLine(L"父进程不播放。提示音只由子进程送到默认播放设备，并循环到子进程退出。");
    outLine(L"请对上面的父进程 PID 做 IncludeTree loopback，确认采集里有没有这段提示音。");
    outLine(L"关掉子进程窗口，或在子进程窗口按 Ctrl+C。父进程会一直等到子进程退出。");

    WaitForSingleObject(child.hProcess, INFINITE);
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    if (job) CloseHandle(job);
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (DeleteFileW(childExe.c_str())) break;
        Sleep(200);
    }
    {
        const size_t slash = childExe.find_last_of(L"\\/");
        if (slash != std::wstring::npos) {
            RemoveDirectoryW(childExe.substr(0, slash).c_str());
        }
    }
    outLine(L"子进程已退出。");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--help" || arg == L"-h" || arg == L"-?" || arg == L"/?") {
            printHelp();
            return 0;
        }
        if (arg == L"--child") return childMain();
    }
    return parentMain();
}
