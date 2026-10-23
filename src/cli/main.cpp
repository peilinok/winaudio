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
#include "AudioEffects.h"
#include "EndpointEnhancements.h"
#include "Elevate.h"
#include "Log.h"

using namespace wa;

static void usage() {
    std::printf(
        "WinAudioCli list  [--render|--capture] [--effects]\n"
        "WinAudioCli capture --out <file.wav> [--device <id>] [--seconds N] [--loopback]\n"
        "                    [--pid N] [--exclude-tree] [--no-silent-render]\n"
        "                    [--backend wasapi-shared|wasapi-exclusive] [--format 48000/16/2]\n"
        "WinAudioCli capture --track --out <file.wav> [--device <id>] [--loopback|--pid N]\n"
        "                    [--exclude-tree] [--format ...] [--no-silent-render]\n"
        "                    [--track --out <file.wav> ...]... [--seconds N] [--backend ...]\n"
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
        "  capture: no --track + one --out = one Capture Track; repeat --track for a group.\n"
        "  each --track needs --out; source is --pid [--exclude-tree] or --loopback or endpoint.\n"
        "  --format and --no-silent-render are per Capture Track; --seconds and --backend once.\n"
        "  --pid and --loopback cannot be combined in one --track segment.\n"
        "WinAudioCli caps  [--device <id>] [--render|--capture]\n"
        "WinAudioCli effects set [--capture|--render] [--device <id>] <effect> on|off\n"
        "  effects set: Shared Initialize + SetAudioEffectState (Win11 build 22000+).\n"
        "  <effect> is a braced GUID, type name, or kebab slug (e.g. noise-suppression).\n"
        "WinAudioCli enhancements set [--capture|--render] [--device <id>] on|off\n"
        "  enhancements set: write PKEY_AudioEndpoint_Disable_SysFx (0=on, 1=off).\n"
        "  Must run before any capture/render stream is started in this process.\n"
        "  ACCESS_DENIED auto-relaunches elevated (UAC); pass --no-elevate to skip.\n");
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
        const bool withEffects = has(argc, argv, L"--effects");
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
            std::printf("     enhancements=%s", enhancementsStateName(d.enhancements));
            if (!d.enhancementOptions.empty()) {
                std::printf("  options=");
                for (size_t i = 0; i < d.enhancementOptions.size(); ++i) {
                    const auto& o = d.enhancementOptions[i];
                    if (i) std::printf(";");
                    std::printf("%s=%s", o.id.c_str(), o.value.c_str());
                }
            }
            std::printf("\n");
            if (withEffects) {
                // On-demand Shared probe (Initialize, no Start). Win11 build 22000+;
                // Win10 / missing IAudioEffectsManager → empty list, not a hard error.
                std::vector<StreamAudioEffect> effects;
                Result er = probeStreamAudioEffects(flow, d.id, effects);
                if (!er) {
                    std::printf("     streamEffects=error (%s)\n", er.message.c_str());
                } else if (effects.empty()) {
                    std::printf("     streamEffects=(none)\n");
                } else {
                    std::printf("     streamEffects=");
                    for (size_t i = 0; i < effects.size(); ++i) {
                        const auto& e = effects[i];
                        if (i) std::printf(";");
                        std::printf("%s:%s%s", e.typeName.c_str(),
                                    audioEffectStateName(e.state),
                                    e.canSetState ? "+set" : "");
                    }
                    std::printf("\n");
                }
            }
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
            const char* shared = s.shared == wa::SupportLevel::Exact ? "exact"
                               : s.shared == wa::SupportLevel::ClosestMatch ? "closest" : "-";
            const char* exclusive = s.exclusive == wa::SupportLevel::Exact ? "exact" : "-";
            std::printf("%-16s %-8s %-9s\n", fmt, shared, exclusive);
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

    if (cmd == L"effects") {
        if (argc < 3 || std::wcscmp(argv[2], L"set") != 0) {
            usage();
            return 1;
        }
        // Positional: <effect> on|off after flags. Default flow = render.
        DataFlow flow = has(argc, argv, L"--capture") ? DataFlow::Capture : DataFlow::Render;
        if (has(argc, argv, L"--capture") && has(argc, argv, L"--render")) {
            std::printf("effects set: use only one of --capture / --render\n");
            return 2;
        }
        DeviceId device = arg(argc, argv, L"--device");

        // Collect non-flag positionals after "effects set".
        std::vector<std::wstring> pos;
        for (int i = 3; i < argc; ++i) {
            if (argv[i][0] == L'-' && argv[i][1] == L'-') {
                // Skip flag and its value when present.
                if (std::wcscmp(argv[i], L"--device") == 0) {
                    if (i + 1 < argc) ++i;
                    continue;
                }
                if (std::wcscmp(argv[i], L"--capture") == 0 ||
                    std::wcscmp(argv[i], L"--render") == 0 ||
                    std::wcscmp(argv[i], L"--log-file") == 0 ||
                    std::wcscmp(argv[i], L"--log-level") == 0) {
                    if ((std::wcscmp(argv[i], L"--log-file") == 0 ||
                         std::wcscmp(argv[i], L"--log-level") == 0) &&
                        i + 1 < argc) {
                        ++i;
                    }
                    continue;
                }
                std::printf("effects set: unknown flag %ls\n", argv[i]);
                return 2;
            }
            pos.push_back(argv[i]);
        }
        if (pos.size() != 2) {
            std::printf("effects set: need <effect> on|off\n");
            return 2;
        }
        const std::string effectTok = narrow(pos[0]);
        const std::string stateTok = narrow(pos[1]);
        const AudioEffectState st = audioEffectStateFromName(stateTok);
        if (st == AudioEffectState::Unknown) {
            std::printf("effects set: state must be on or off (got %s)\n", stateTok.c_str());
            return 2;
        }

        std::vector<StreamAudioEffect> after;
        Result r = setStreamAudioEffect(flow, device, effectTok, st, &after);
        if (!r) {
            std::printf("effects set failed: %s\n", r.message.c_str());
            return 2;
        }
        std::printf("effects set ok: %s -> %s (%s)\n",
                    effectTok.c_str(), audioEffectStateName(st),
                    flow == DataFlow::Capture ? "capture" : "render");
        if (after.empty()) {
            std::printf("streamEffects=(none)\n");
        } else {
            std::printf("streamEffects=");
            for (size_t i = 0; i < after.size(); ++i) {
                const auto& e = after[i];
                if (i) std::printf(";");
                std::printf("%s:%s%s", e.typeName.c_str(),
                            audioEffectStateName(e.state),
                            e.canSetState ? "+set" : "");
            }
            std::printf("\n");
        }
        return 0;
    }


    if (cmd == L"enhancements") {
        if (argc < 3 || std::wcscmp(argv[2], L"set") != 0) {
            usage();
            return 1;
        }
        DataFlow flow = has(argc, argv, L"--capture") ? DataFlow::Capture : DataFlow::Render;
        if (has(argc, argv, L"--capture") && has(argc, argv, L"--render")) {
            std::printf("enhancements set: use only one of --capture / --render\n");
            return 2;
        }
        DeviceId device = arg(argc, argv, L"--device");
        const bool noElevate = has(argc, argv, L"--no-elevate");

        std::vector<std::wstring> pos;
        for (int i = 3; i < argc; ++i) {
            if (argv[i][0] == L'-' && argv[i][1] == L'-') {
                if (std::wcscmp(argv[i], L"--device") == 0) {
                    if (i + 1 < argc) ++i;
                    continue;
                }
                if (std::wcscmp(argv[i], L"--capture") == 0 ||
                    std::wcscmp(argv[i], L"--render") == 0 ||
                    std::wcscmp(argv[i], L"--no-elevate") == 0) {
                    continue;
                }
                if (std::wcscmp(argv[i], L"--log-file") == 0 ||
                    std::wcscmp(argv[i], L"--log-level") == 0) {
                    if (i + 1 < argc) ++i;
                    continue;
                }
                std::printf("enhancements set: unknown flag %ls\n", argv[i]);
                return 2;
            }
            pos.push_back(argv[i]);
        }
        if (pos.size() != 1) {
            std::printf("enhancements set: need on|off\n");
            return 2;
        }
        const std::string stateTok = narrow(pos[0]);
        const EnhancementsState st = enhancementsStateFromName(stateTok);
        if (st == EnhancementsState::Unknown) {
            std::printf("enhancements set: state must be on or off (got %s)\n", stateTok.c_str());
            return 2;
        }

        DeviceInfo after{};
        Result r = setEndpointEnhancements(flow, device, st, &after);
        if (!r && resultIsAccessDenied(r) && !noElevate && !processIsElevated()) {
            // Rebuild params for an elevated self-relaunch; --no-elevate prevents a loop.
            std::wstring params = L"enhancements set --no-elevate ";
            params += (flow == DataFlow::Capture) ? L"--capture " : L"--render ";
            if (!device.empty()) {
                params += L"--device \"";
                params += device;
                params += L"\" ";
            }
            params += pos[0];
            wchar_t self[MAX_PATH] = {};
            const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
            if (n == 0 || n >= MAX_PATH) {
                std::printf("enhancements set failed: %s (and cannot self-elevate)\n",
                            r.message.c_str());
                return 2;
            }
            std::printf("enhancements set: ACCESS_DENIED — requesting elevation...\n");
            unsigned long exitCode = 1;
            Result er = runElevatedAndWait(self, params, &exitCode);
            if (!er) {
                std::printf("enhancements set elevation failed: %s\n", er.message.c_str());
                return 2;
            }
            // Child already printed success/failure; refresh for parent display.
            DeviceEnumerator de;
            if (device.empty()) {
                de.defaultDevice(flow, after);
            } else {
                std::vector<DeviceInfo> all;
                if (de.enumerate(flow, all)) {
                    for (auto& d : all) {
                        if (d.id == device) { after = std::move(d); break; }
                    }
                }
            }
            std::printf("enhancements set ok (elevated): %s (%s)\n",
                        enhancementsStateName(st),
                        flow == DataFlow::Capture ? "capture" : "render");
            std::printf("enhancements=%s\n", enhancementsStateName(after.enhancements));
            return 0;
        }
        if (!r) {
            std::printf("enhancements set failed: %s\n", r.message.c_str());
            return 2;
        }
        std::printf("enhancements set ok: %s (%s)\n",
                    enhancementsStateName(st),
                    flow == DataFlow::Capture ? "capture" : "render");
        std::printf("enhancements=%s\n", enhancementsStateName(after.enhancements));
        return 0;
    }

    usage();
    return 1;
}
