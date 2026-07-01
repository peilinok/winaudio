#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "Engine.h"
#include "DeviceEnumerator.h"
#include "ComUtil.h"
#include "FormatSpec.h"
#include "MonitorEngine.h"

using namespace wa;

static void usage() {
    std::printf(
        "WinAudioCli list  [--render|--capture]\n"
        "WinAudioCli capture --out <file.wav> [--device <id>] [--seconds N]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive] [--format 48000/16/2]\n"
        "WinAudioCli play    --in  <file.wav> [--device <id>]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive]\n"
        "WinAudioCli probe   --format 48000/16/2 [--device <id>] [--render|--capture]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive]\n"
        "WinAudioCli monitor [--cap <id>] [--render <id>] [--delay-ms N] [--seconds N]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive]\n");
}

static const char* stateStr(wa::StreamState st) {
    switch (st) {
    case wa::StreamState::Idle:    return "Idle";
    case wa::StreamState::Running: return "Running";
    case wa::StreamState::Error:   return "Error";
    default:                       return "?";
    }
}

static std::wstring arg(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i < argc - 1; ++i)
        if (std::wcscmp(argv[i], key) == 0) return argv[i + 1];
    return L"";
}
static bool has(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i < argc; ++i) if (std::wcscmp(argv[i], key) == 0) return true;
    return false;
}

static std::string narrow(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s += static_cast<char>(c); // format spec is ASCII
    return s;
}
static BackendKind backendArg(int argc, wchar_t** argv) {
    std::wstring b = arg(argc, argv, L"--backend");
    return (b == L"wasapi-exclusive") ? BackendKind::WasapiExclusive
                                      : BackendKind::WasapiShared;
}
// Returns true and fills `fmt` if --format present & valid; false if absent; exits(2) if invalid.
static bool formatArg(int argc, wchar_t** argv, AudioFormat& fmt) {
    std::wstring f = arg(argc, argv, L"--format");
    if (f.empty()) return false;
    if (!parseFormatSpec(narrow(f), fmt)) {
        std::printf("invalid --format (want R/B/C[f], e.g. 48000/16/2)\n");
        std::exit(2);
    }
    return true;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { usage(); return 1; }
    ComInitGuard com;
    std::wstring cmd = argv[1];

    if (cmd == L"list") {
        DataFlow flow = has(argc, argv, L"--capture") ? DataFlow::Capture : DataFlow::Render;
        DeviceEnumerator de;
        std::vector<DeviceInfo> devs;
        Result r = de.enumerate(flow, devs);
        if (!r) { std::printf("enumerate failed: %s\n", r.message.c_str()); return 2; }
        for (auto& d : devs) {
            std::wprintf(L"%s %s  [%u Hz %u ch %s]\n",
                d.isDefault ? L"*" : L" ", d.name.c_str(),
                d.mixFormat.sampleRate, d.mixFormat.channels,
                d.mixFormat.isFloat ? L"float" : L"pcm");
            std::wprintf(L"     id=%s\n", d.id.c_str());
        }
        return 0;
    }

    Engine eng;
    if (cmd == L"capture") {
        std::wstring out = arg(argc, argv, L"--out");
        if (out.empty()) { usage(); return 1; }
        std::wstring id = arg(argc, argv, L"--device");
        std::wstring secStr = arg(argc, argv, L"--seconds");
        int seconds = secStr.empty() ? 5 : _wtoi(secStr.c_str());
        AudioFormat fmt{};
        bool haveFmt = formatArg(argc, argv, fmt);
        if (haveFmt && backendArg(argc, argv) != BackendKind::WasapiExclusive) {
            std::printf("--format only applies to --backend wasapi-exclusive "
                        "(shared mode uses the device mix format)\n");
            return 2;
        }
        Result r = eng.startCapture(backendArg(argc, argv), id, out,
                                    haveFmt ? &fmt : nullptr);
        if (!r) { std::printf("capture start failed: %s\n", r.message.c_str()); return 2; }
        for (int i = 0; i < seconds * 10; ++i) {
            Sleep(100);
            EngineStatus s = eng.poll();
            std::printf("\rL=%.2f R=%.2f over=%llu under=%llu  ",
                s.levelL, s.levelR,
                (unsigned long long)s.overruns, (unsigned long long)s.underruns);
        }
        eng.stop();
        std::printf("\nwrote %ls\n", out.c_str());
        return 0;
    }

    if (cmd == L"play") {
        std::wstring in = arg(argc, argv, L"--in");
        if (in.empty()) { usage(); return 1; }
        if (has(argc, argv, L"--format")) {
            std::printf("play: --format is not used (the format is read from the WAV file)\n");
            return 2;
        }
        std::wstring id = arg(argc, argv, L"--device");
        Result r = eng.startPlayback(backendArg(argc, argv), id, in);
        if (!r) { std::printf("play start failed: %s\n", r.message.c_str()); return 2; }
        for (;;) {
            Sleep(100);
            EngineStatus s = eng.poll();
            if (s.state == EngineState::Idle || s.state == EngineState::Error) break;
            std::printf("\rplaying L=%.2f R=%.2f  ", s.levelL, s.levelR);
        }
        EngineStatus fin = eng.poll();
        eng.stop();
        if (fin.state == EngineState::Error) {
            std::printf("\nplay failed: %s\n", fin.message.c_str());
            return 2;
        }
        std::printf("\ndone\n");
        return 0;
    }

    if (cmd == L"probe") {
        AudioFormat fmt{};
        if (!formatArg(argc, argv, fmt)) { usage(); return 1; }
        std::wstring id = arg(argc, argv, L"--device");
        DataFlow flow = has(argc, argv, L"--capture") ? DataFlow::Capture : DataFlow::Render;
        Result r = eng.probeFormat(backendArg(argc, argv), flow, id, fmt);
        std::printf("%s: %s\n", r ? "SUPPORTED" : "NOT SUPPORTED", r.message.c_str());
        return r ? 0 : 1;
    }

    if (cmd == L"monitor") {
        std::wstring capId    = arg(argc, argv, L"--cap");
        std::wstring renderId = arg(argc, argv, L"--render");
        std::wstring delayStr = arg(argc, argv, L"--delay-ms");
        std::wstring secStr   = arg(argc, argv, L"--seconds");
        int dms = delayStr.empty() ? 100 : _wtoi(delayStr.c_str());
        if (dms < 0) dms = 0;
        uint32_t delayMs = static_cast<uint32_t>(dms);
        int seconds      = secStr.empty()   ? 5   : _wtoi(secStr.c_str());

        wa::MonitorEngine mon;
        wa::Result r = mon.start(backendArg(argc, argv), capId, renderId, delayMs);
        if (!r) { std::printf("monitor start failed: %s\n", r.message.c_str()); return 2; }
        for (int i = 0; i < seconds * 5; ++i) {
            Sleep(200);
            wa::MonitorStatus s = mon.poll();
            std::printf("\rcap=%s ren=%s  sr=%u  fifo=%.0fms  drift=%llu  xrun c/r=%llu/%llu   ",
                stateStr(s.capState), stateStr(s.renderState), s.sampleRate,
                s.fifoFillMs, (unsigned long long)s.driftFixes,
                (unsigned long long)s.capXruns, (unsigned long long)s.renderXruns);
            std::fflush(stdout);
            if (s.overall == wa::StreamState::Error) {
                std::printf("\nerr=%u\n", s.errorCode);
                mon.stop();
                return 2;
            }
        }
        mon.stop();
        std::printf("\ndone\n");
        return 0;
    }

    usage();
    return 1;
}
