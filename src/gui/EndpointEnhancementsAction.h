#pragma once
#include "IAudioBackend.h"
#include <optional>
#include <string>
#include <string_view>

namespace wa::endpoint_enhancements_action {

inline bool requested(int argc, wchar_t* const* argv) {
    return argc > 1 && std::wstring_view(argv[1]) == L"--internal-endpoint-enhancements";
}

// Quote one Windows argument, including quotes and trailing backslashes.
inline std::wstring arguments(DataFlow flow, const DeviceId& device, EnhancementsState state) {
    std::wstring out = L"--internal-endpoint-enhancements ";
    out += flow == DataFlow::Capture ? L"capture " : L"render ";
    out += state == EnhancementsState::Enabled ? L"on \"" : L"off \"";
    size_t slashes = 0;
    for (wchar_t c : device) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        out += c;
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out += L'"';
    return out;
}

template<class Apply>
std::optional<int> handle(int argc, wchar_t* const* argv, bool elevated, Apply apply) {
    if (!requested(argc, argv)) return std::nullopt;
    if (argc != 5) return 2;
    const std::wstring_view direction(argv[2]);
    const std::wstring_view state(argv[3]);
    if ((direction != L"capture" && direction != L"render") ||
        (state != L"on" && state != L"off")) return 2;
    // The parent owns UAC. A child must never retry elevation.
    if (!elevated) return 5; // ERROR_ACCESS_DENIED
    return apply(direction == L"capture" ? DataFlow::Capture : DataFlow::Render,
                 DeviceId(argv[4]),
                 state == L"on" ? EnhancementsState::Enabled : EnhancementsState::Disabled);
}

} // namespace wa::endpoint_enhancements_action
