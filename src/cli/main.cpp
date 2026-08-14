#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include "CaptureTrackList.h"
#include "Engine.h"
#include "DeviceEnumerator.h"
#include "ComUtil.h"
#include "CliOptions.h"
#include "FormatSpec.h"
#include "MonitorEngine.h"
#include "Capabilities.h"
#include "Log.h"

using namespace wa;

static void usage() {
    std::printf(
        "WinAudioCli list  [--render|--capture]\n"
        "WinAudioCli capture --out <file.wav> [--device <id>] [--seconds N] [--loopback]\n"
        "                    [--pid N] [--exclude-tree] [--no-silent-render]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive] [--format 48000/16/2]\n"
        "                    [--track --out <file.wav> [--device <id>] [--loopback|--pid N]\n"
        "                              [--exclude-tree] [--format ...] [--no-silent-render]]...\n"
        "WinAudioCli play    --in  <file.wav> [--device <id>]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive]\n"
        "WinAudioCli probe   --format 48000/16/2 [--device <id>] [--render|--capture]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive]\n"
        "WinAudioCli monitor [--loopback] [--cap <id>] [--render <id>] [--delay-ms N] [--seconds N]\n"
        "                    [--no-silent-render] [--backend wasapi-shared|wasapi-exclusive]\n"
        "                    [--format R/B/C[f]]\n"
        "  (shared: WASAPI engine bridges sample rate on render side;\n"
        "   exclusive: render device must support capture format)\n"
        "  --loopback uses render endpoint ids for capture --device / monitor --cap.\n"
        "  capture --track starts one Capture Track per segment; omit --track for one --out.\n"
        "WinAudioCli caps  [--device <id>] [--render|--capture]\n");
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

struct LogShutdown { ~LogShutdown() { wa::log::shutdown(); } };

// Logging: stderr sink always; optional --log-file; level from --log-level (default info).
// stderr keeps the \r status lines (stdout) clean for redirection.
static void initLogging(int argc, wchar_t** argv) {
    wa::log::init();
    wa::log::setThreadName("main");
    wa::log::addStderrSink();
    std::wstring lf = arg(argc, argv, L"--log-file");
    if (!lf.empty()) wa::log::addFileSink(narrow(lf));
    std::wstring lv = arg(argc, argv, L"--log-level");
    wa::log::Level lvl = wa::log::Level::Info;
    if      (lv == L"trace") lvl = wa::log::Level::Trace;
    else if (lv == L"debug") lvl = wa::log::Level::Debug;
    else if (lv == L"warn")  lvl = wa::log::Level::Warn;
    else if (lv == L"err" || lv == L"error") lvl = wa::log::Level::Err;
    wa::log::setLevel(lvl);
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { usage(); return 1; }
    ComInitGuard com;
    LogShutdown logShutdownGuard;
    initLogging(argc, argv);
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
        auto group = wa::cli::parseCaptureGroup(argc, argv);
        if (!group.ok) {
            std::printf("%s\n", group.message.c_str());
            return 2;
        }
        CaptureTrackList list;
        static CaptureTrackList* gCaptureList = nullptr;
        gCaptureList = &list;
        SetConsoleCtrlHandler([](DWORD) -> BOOL {
            if (gCaptureList) gCaptureList->destroyAll();
            return FALSE;
        }, TRUE);
        bool anyCreateFail = false;
        std::vector<std::wstring> outs;
        outs.reserve(group.tracks.size());
        for (const auto& seg : group.tracks) {
            auto spec = wa::cli::captureCreateFromSegment(seg, group.backend);
            TrackId id = 0;
            Result r = list.create(spec, &id);
            if (!r) {
                std::printf("capture start failed: %s\n", r.message.c_str());
                anyCreateFail = true;
                continue;
            }
            outs.push_back(seg.out);
        }
        if (list.poll().empty()) return 2;
        bool anyError = anyCreateFail;
        std::string err;
        for (int i = 0; i < group.seconds * 10; ++i) {
            Sleep(100);
            auto st = list.poll();
            float l = 0.f, rlev = 0.f;
            uint64_t ov = 0;
            for (const auto& s : st) {
                l = s.levelL;
                rlev = s.levelR;
                ov += s.overruns;
                if (s.state == StreamState::Error) {
                    anyError = true;
                    if (!s.message.empty()) err = s.message;
                }
            }
            std::printf("\rtracks=%zu L=%.2f R=%.2f over=%llu  ",
                st.size(), l, rlev, (unsigned long long)ov);
        }
        list.destroyAll();
        if (anyError) {
            std::printf("\ncapture error: %s\n", err.c_str());
            return 2;
        }
        for (const auto& p : outs) std::printf("\nwrote %ls", p.c_str());
        std::printf("\n");
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

    if (cmd == L"caps") {
        DataFlow flow = has(argc, argv, L"--capture") ? DataFlow::Capture : DataFlow::Render;
        DeviceId id = arg(argc, argv, L"--device");
        DeviceEnumerator de;
        wa::DeviceCapabilities caps;
        wa::Result r = de.queryCapabilities(flow, id, caps);
        if (!r) { std::printf("caps failed: %s\n", r.message.c_str()); return 2; }
        auto pf = [](const char* tag, bool has, const AudioFormat& f){
            if (has) std::printf("%s: %u/%u/%u%s\n", tag, f.sampleRate, f.bitsPerSample, f.channels, f.isFloat?"f":"");
            else     std::printf("%s: (none)\n", tag);
        };
        pf("Mix",    caps.hasMix,    caps.mixFormat);
        pf("Device", caps.hasDevice, caps.deviceFormat);
        pf("OEM",    caps.hasOem,    caps.oemFormat);
        std::printf("%-16s %-8s %-9s\n", "Format", "Shared", "Exclusive");
        for (const auto& s : caps.matrix) {
            char fmt[32];
            std::snprintf(fmt, sizeof fmt, "%u/%u/%u%s", s.fmt.sampleRate, s.fmt.bitsPerSample, s.fmt.channels, s.fmt.isFloat?"f":"");
            std::printf("%-16s %-8s %-9s\n", fmt, s.sharedOk?"yes":"-", s.exclusiveOk?"yes":"-");
        }
        return 0;
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
        AudioFormat capFmt{};
        bool haveFmt = formatArg(argc, argv, capFmt);
        const bool loopback = has(argc, argv, L"--loopback");
        BackendKind kind = backendArg(argc, argv);
        if (loopback && kind == BackendKind::WasapiExclusive) {
            std::printf("monitor: --loopback requires --backend wasapi-shared\n");
            return 2;
        }
        CaptureSource source{loopback ? CaptureSourceKind::SystemLoopback : CaptureSourceKind::Endpoint,
                             capId};
        LoopbackOptions loopbackOptions = wa::cli::parseLoopbackOptions(argc, argv);

        wa::MonitorEngine mon;
        wa::Result r = mon.start(kind, source, renderId, delayMs,
                                 true, {}, {}, haveFmt ? &capFmt : nullptr,
                                 loopbackOptions);
        if (!r) { std::printf("monitor start failed: %s\n", r.message.c_str()); return 2; }
        bool sawErr = false;
        uint32_t errCode = 0;
        for (int i = 0; i < seconds * 5; ++i) {
            Sleep(200);
            wa::MonitorStatus s = mon.poll();
            std::printf("\rcap=%s ren=%s  sr=%u  fifo=%.0fms  drift=%llu  xrun c/r=%llu/%llu   ",
                stateStr(s.capState), stateStr(s.renderState), s.sampleRate,
                s.fifoFillMs, (unsigned long long)s.driftFixes,
                (unsigned long long)s.capXruns, (unsigned long long)s.renderXruns);
            std::fflush(stdout);
            if (s.overall == wa::StreamState::Error && !sawErr) {
                std::printf("\nerr=%u\n", s.errorCode);
                sawErr = true;
                errCode = s.errorCode;
            }
        }
        mon.stop();
        if (sawErr) {
            std::printf("monitor ended with err=%u\n", errCode);
            return 2;
        }
        std::printf("\ndone\n");
        return 0;
    }

    usage();
    return 1;
}
