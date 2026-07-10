#pragma once
#include <cwchar>
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

} // namespace wa::cli
