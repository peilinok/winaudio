#include "HookedCall.h"
#include "HookedCallPod.h"
#include <algorithm>
#include <cstddef>
#include <cstring>

namespace wa {

bool decodeCallArgBlob(const uint8_t* data, uint16_t len, std::vector<CallArg>& out) {
    out.clear();
    if (!data || len < 2) return false;
    bool ok = hook_ipc::decodeArgBlob(data, len,
        [&](uint8_t depth, uint8_t dir, const char* type, uint8_t tl, const char* name,
            uint8_t nl, const char* pre, uint8_t pl, const char* post, uint8_t ql) {
            CallArg a;
            a.depth = depth;
            a.dir = dir;
            a.type.assign(type, type + tl);
            a.name.assign(name, name + nl);
            a.pre.assign(pre, pre + pl);
            a.post.assign(post, post + ql);
            out.push_back(std::move(a));
        });
    return ok && !out.empty();
}

const char* callArgDirName(uint8_t dir) {
    switch (dir) {
        case hook_ipc::kArgOut: return "out";
        case hook_ipc::kArgInOut: return "inout";
        case hook_ipc::kArgRet: return "return";
        case hook_ipc::kArgIn:
        default: return "in";
    }
}

bool isPumpMethod(const std::string& method) {
    return method == "GetBuffer" || method == "ReleaseBuffer";
}

std::string sanitizeHookedArgs(std::string args) {
    const auto pos = args.find("pcm=");
    if (pos != std::string::npos) {
        args.resize(pos);
        while (!args.empty() && (args.back() == ' ' || args.back() == ','))
            args.pop_back();
    }
    return args;
}

CallLogView shapeCallLog(const std::vector<HookedCall>& in, bool pumpEnabled, size_t pumpCap) {
    CallLogView view;
    std::vector<HookedCall> pump;
    view.entries.reserve(in.size());
    pump.reserve(pumpEnabled ? std::min(in.size(), pumpCap + 8) : 0);
    for (HookedCall c : in) {
        const bool isPump = c.pump || isPumpMethod(c.method);
        c.pump = isPump;
        c.args = sanitizeHookedArgs(std::move(c.args));
        if (isPump) {
            if (pumpEnabled) {
                if (c.xrun) ++view.pumpXruns;
                pump.push_back(std::move(c));
            }
            continue;
        }
        view.entries.push_back(std::move(c));
    }
    if (pumpCap > 0 && pump.size() > pumpCap) {
        pump.erase(pump.begin(),
                   pump.begin() + static_cast<std::ptrdiff_t>(pump.size() - pumpCap));
    }
    view.entries.insert(view.entries.end(), pump.begin(), pump.end());
    std::stable_sort(view.entries.begin(), view.entries.end(),
                     [](const HookedCall& a, const HookedCall& b) { return a.timeMs < b.timeMs; });
    return view;
}

EtwInitializeHint extractHookedInitialize(const std::vector<HookedCall>& calls) {
    EtwInitializeHint h;
    for (const auto& c : calls) {
        if (c.pump || isPumpMethod(c.method)) continue;
        if (c.method != "Initialize" && c.method != "SetClientProperties" &&
            c.method != "InitializeSharedAudioStream")
            continue;
        h.present = true;
        if (c.category) h.category = c.category;
        if (c.raw) h.raw = c.raw;
        if (c.matchFormat) h.matchFormat = c.matchFormat;
        if (c.exclusive) h.exclusive = c.exclusive;
        if (c.method == "Initialize" || c.method == "InitializeSharedAudioStream")
            h.hresult = c.hresult;
        if (c.format) h.format = c.format;
    }
    return h;
}

EtwInitializeHint mergeInitializeHint(const EtwInitializeHint& etw,
                                      const EtwInitializeHint& hooked) {
    if (!hooked.present) return etw;
    EtwInitializeHint out = etw;
    out.present = true;
    if (hooked.category) out.category = hooked.category;
    if (hooked.raw) out.raw = hooked.raw;
    if (hooked.matchFormat) out.matchFormat = hooked.matchFormat;
    if (hooked.exclusive) out.exclusive = hooked.exclusive;
    if (hooked.hresult) out.hresult = hooked.hresult;
    if (hooked.format) out.format = hooked.format;
    return out;
}

}  // namespace wa
