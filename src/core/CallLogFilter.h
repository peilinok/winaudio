#pragma once
#include "PipelineGraph.h"
#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace wa::call_log_filter {

inline char asciiLower(unsigned char c) {
    return static_cast<char>(std::tolower(c));
}

inline bool asciiEqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (asciiLower(static_cast<unsigned char>(a[i])) !=
            asciiLower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

// Case-insensitive subsequence match (e.g. "iacl" -> "IAudioClient::Initialize").
inline bool fuzzySubsequence(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    size_t hi = 0;
    for (char nc : needle) {
        const char n = asciiLower(static_cast<unsigned char>(nc));
        bool found = false;
        while (hi < hay.size()) {
            if (asciiLower(static_cast<unsigned char>(hay[hi++])) == n) {
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

// Whitespace-separated tokens; each token must subsequence-match hay (independently).
inline bool fuzzyMatch(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    size_t i = 0;
    while (i < needle.size()) {
        while (i < needle.size() && std::isspace(static_cast<unsigned char>(needle[i]))) ++i;
        if (i >= needle.size()) break;
        const size_t start = i;
        while (i < needle.size() && !std::isspace(static_cast<unsigned char>(needle[i]))) ++i;
        if (!fuzzySubsequence(hay, needle.substr(start, i - start))) return false;
    }
    return true;
}

inline std::string formatCallName(const HookedCall& c) {
    std::string name = c.iface;
    if (!c.iface.empty() && !c.method.empty()) name += "::";
    name += c.method;
    return name;
}

// Presentation only: matching and export continue to use the original summary.
inline std::string singleLine(std::string text) {
    for (auto& c : text) if (c == '\n' || c == '\r') c = ' ';
    return text;
}

inline bool callMatches(const HookedCall& c, std::string_view filter) {
    if (filter.empty()) return true;
    if (fuzzyMatch(formatCallName(c), filter)) return true;
    if (fuzzyMatch(c.args, filter)) return true;
    return false;
}

// Combined Call Log filters for debugging. Empty iface => all interfaces.
// failuresOnly keeps rows whose HRESULT is not S_OK (0).
struct FilterOpts {
    std::string_view text;
    std::string_view iface;
    bool failuresOnly = false;
};

inline bool callMatches(const HookedCall& c, const FilterOpts& opts) {
    if (opts.failuresOnly && c.hresult == 0) return false;
    if (!opts.iface.empty() && !asciiEqualsIgnoreCase(c.iface, opts.iface)) return false;
    return callMatches(c, opts.text);
}

inline void addIface(std::vector<std::string>& out, std::string_view iface) {
    if (iface.empty()) return;
    for (const auto& s : out) if (asciiEqualsIgnoreCase(s, iface)) return;
    out.emplace_back(iface);
}

inline void sortIfaces(std::vector<std::string>& out) {
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
        const size_t n = (std::min)(a.size(), b.size());
        for (size_t i = 0; i < n; ++i) {
            const char ca = asciiLower(static_cast<unsigned char>(a[i]));
            const char cb = asciiLower(static_cast<unsigned char>(b[i]));
            if (ca != cb) return ca < cb;
        }
        return a.size() < b.size();
    });
}

// Unique iface names, sorted case-insensitively for chip/combo UI.
inline std::vector<std::string> uniqueIfaces(const std::vector<HookedCall>& calls) {
    std::vector<std::string> out;
    for (const auto& c : calls) addIface(out, c.iface);
    sortIfaces(out);
    return out;
}

// Indices into `calls` that match filter (empty filter => all indices).
inline std::vector<size_t> matchingIndices(const std::vector<HookedCall>& calls,
                                           std::string_view filter) {
    FilterOpts opts;
    opts.text = filter;
    std::vector<size_t> out;
    out.reserve(calls.size());
    for (size_t i = 0; i < calls.size(); ++i) {
        if (callMatches(calls[i], opts)) out.push_back(i);
    }
    return out;
}

inline std::vector<size_t> matchingIndices(const std::vector<HookedCall>& calls,
                                           const FilterOpts& opts) {
    std::vector<size_t> out;
    out.reserve(calls.size());
    for (size_t i = 0; i < calls.size(); ++i) {
        if (callMatches(calls[i], opts)) out.push_back(i);
    }
    return out;
}

}  // namespace wa::call_log_filter
