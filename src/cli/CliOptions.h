#pragma once
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>
#include "CaptureTrackList.h"
#include "FormatSpec.h"
#include "IAudioBackend.h"

namespace wa::cli {

inline bool hasArg(int argc, wchar_t** argv, const wchar_t* key) {
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], key) == 0) return true;
    }
    return false;
}

inline LoopbackOptions parseLoopbackOptions(int argc, wchar_t** argv) {
    LoopbackOptions opts{};
    opts.silentRender = !hasArg(argc, argv, L"--no-silent-render");
    return opts;
}

struct CaptureSegment {
    std::wstring out;
    std::wstring device;
    bool         loopback = false;
    bool         hasPid = false;
    uint32_t     pid = 0;
    bool         excludeTree = false;
    bool         hasFormat = false;
    AudioFormat  format{};
    bool         noSilentRender = false;
};

struct CaptureGroupParse {
    bool ok = true;
    std::string message;
    BackendKind backend = BackendKind::WasapiShared;
    int seconds = 5;
    std::vector<CaptureSegment> tracks;
};

inline int indexOf(int begin, int end, wchar_t** argv, const wchar_t* key) {
    for (int i = begin; i < end; ++i) {
        if (std::wcscmp(argv[i], key) == 0) return i;
    }
    return -1;
}

inline bool hasIn(int begin, int end, wchar_t** argv, const wchar_t* key) {
    return indexOf(begin, end, argv, key) >= 0;
}

inline std::wstring argIn(int begin, int end, wchar_t** argv, const wchar_t* key) {
    const int i = indexOf(begin, end, argv, key);
    if (i < 0 || i + 1 >= end) return L"";
    return argv[i + 1];
}

inline std::string narrowAscii(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s += static_cast<char>(c);
    return s;
}

inline bool parseSegment(int begin, int end, wchar_t** argv, CaptureSegment& seg,
                         std::string& err) {
    seg.out = argIn(begin, end, argv, L"--out");
    if (seg.out.empty()) {
        err = "capture: each track requires --out";
        return false;
    }
    seg.device = argIn(begin, end, argv, L"--device");
    seg.loopback = hasIn(begin, end, argv, L"--loopback");
    seg.excludeTree = hasIn(begin, end, argv, L"--exclude-tree");
    seg.noSilentRender = hasIn(begin, end, argv, L"--no-silent-render");

    const int pidAt = indexOf(begin, end, argv, L"--pid");
    if (pidAt >= 0) {
        if (pidAt + 1 >= end) {
            err = "capture: --pid requires a process id";
            return false;
        }
        wchar_t* endp = nullptr;
        const unsigned long v = std::wcstoul(argv[pidAt + 1], &endp, 10);
        if (!endp || *endp != L'\0' || v == 0) {
            err = "capture: --pid must be a positive integer";
            return false;
        }
        seg.hasPid = true;
        seg.pid = static_cast<uint32_t>(v);
    }
    if (seg.hasPid && seg.loopback) {
        err = "capture: --pid and --loopback cannot be combined in one track";
        return false;
    }

    const std::wstring fmt = argIn(begin, end, argv, L"--format");
    if (!fmt.empty()) {
        if (!parseFormatSpec(narrowAscii(fmt), seg.format)) {
            err = "invalid --format (want R/B/C[f], e.g. 48000/16/2)";
            return false;
        }
        seg.hasFormat = true;
    }
    return true;
}

inline CaptureGroupParse parseCaptureGroup(int argc, wchar_t** argv) {
    CaptureGroupParse out;
    const int start = (argc >= 2) ? 2 : 1;

    const std::wstring backend = argIn(start, argc, argv, L"--backend");
    if (backend == L"wasapi-exclusive") out.backend = BackendKind::WasapiExclusive;

    const std::wstring sec = argIn(start, argc, argv, L"--seconds");
    if (!sec.empty()) out.seconds = static_cast<int>(std::wcstol(sec.c_str(), nullptr, 10));
    if (out.seconds < 0) out.seconds = 0;

    std::vector<int> marks;
    for (int i = start; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--track") == 0) marks.push_back(i);
    }

    if (marks.empty()) {
        CaptureSegment seg;
        if (!parseSegment(start, argc, argv, seg, out.message)) {
            out.ok = false;
            return out;
        }
        out.tracks.push_back(std::move(seg));
        return out;
    }

    for (size_t i = 0; i < marks.size(); ++i) {
        const int begin = marks[i] + 1;
        const int end = (i + 1 < marks.size()) ? marks[i + 1] : argc;
        CaptureSegment seg;
        if (!parseSegment(begin, end, argv, seg, out.message)) {
            out.ok = false;
            return out;
        }
        out.tracks.push_back(std::move(seg));
    }
    return out;
}

inline CaptureTrackCreate captureCreateFromSegment(const CaptureSegment& seg, BackendKind kind) {
    CaptureTrackCreate spec{};
    spec.kind = kind;
    spec.wavPath = seg.out;
    spec.loopbackOptions.silentRender = !seg.noSilentRender;
    spec.requested = seg.hasFormat ? &seg.format : nullptr;
    if (seg.hasPid) {
        spec.source.kind = CaptureSourceKind::ApplicationLoopback;
        spec.source.processId = seg.pid;
        spec.source.processLoopbackMode = seg.excludeTree ? ProcessLoopbackMode::ExcludeTree
                                                          : ProcessLoopbackMode::IncludeTree;
    } else if (seg.loopback) {
        spec.source.kind = CaptureSourceKind::SystemLoopback;
        spec.source.deviceId = seg.device;
    } else {
        spec.source.kind = CaptureSourceKind::Endpoint;
        spec.source.deviceId = seg.device;
    }
    return spec;
}

} // namespace wa::cli

