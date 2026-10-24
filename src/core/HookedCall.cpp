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
    if (!ok) out.clear();
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

namespace {

bool isInitializeLike(const HookedCall& c) {
    return c.method == "Initialize" || c.method == "InitializeSharedAudioStream";
}

bool isInitializeContributor(const HookedCall& c) {
    if (c.pump || isPumpMethod(c.method)) return false;
    return isInitializeLike(c) || c.method == "SetClientProperties";
}

void applyContributor(EtwInitializeHint& h, const HookedCall& c) {
    h.present = true;
    if (c.category) h.category = c.category;
    if (c.raw) h.raw = c.raw;
    if (c.matchFormat) h.matchFormat = c.matchFormat;
    if (c.exclusive) h.exclusive = c.exclusive;
    if (isInitializeLike(c)) h.hresult = c.hresult;
    if (c.format) h.format = c.format;
}

}  // namespace

HookedInitializeExtract extractHookedInitializeRefs(const std::vector<const HookedCall*>& calls) {
    HookedInitializeExtract out;
    const HookedCall* latestInit = nullptr;
    std::vector<uint32_t> streamIds;
    bool anyZeroInit = false;
    for (const auto* call : calls) {
        const auto& c = *call;
        if (c.pump || isPumpMethod(c.method)) continue;
        if (!isInitializeLike(c)) continue;
        if (!latestInit || c.timeMs >= latestInit->timeMs) latestInit = &c;
        if (c.streamId == 0) {
            anyZeroInit = true;
            continue;
        }
        if (std::find(streamIds.begin(), streamIds.end(), c.streamId) == streamIds.end())
            streamIds.push_back(c.streamId);
    }
    out.streamCount = streamIds.size() + (anyZeroInit ? 1u : 0u);
    if (out.streamCount == 0 && latestInit) out.streamCount = 1;
    if (!latestInit) return out;

    out.primaryStreamId = latestInit->streamId;
    for (const auto* call : calls) {
        const auto& c = *call;
        if (!isInitializeContributor(c)) continue;
        // Non-zero primary: only that stream. Zero primary: legacy last-wins across all.
        if (out.primaryStreamId != 0 && c.streamId != out.primaryStreamId) continue;
        applyContributor(out.hint, c);
    }
    return out;
}

HookedInitializeExtract extractHookedInitializeDetailed(const std::vector<HookedCall>& calls) {
    std::vector<const HookedCall*> refs;
    refs.reserve(calls.size());
    for (const auto& call : calls) refs.push_back(&call);
    return extractHookedInitializeRefs(refs);
}

EtwInitializeHint extractHookedInitialize(const std::vector<HookedCall>& calls) {
    return extractHookedInitializeDetailed(calls).hint;
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

std::vector<HookedCall> filterHookedCallsForSession(const std::vector<HookedCall>& calls,
                                                    uint32_t sessionPid) {
    std::vector<HookedCall> out;
    out.reserve(calls.size());
    for (const auto& c : calls) {
        if (c.pid == 0 || c.pid == sessionPid) out.push_back(c);
    }
    return out;
}

PipelineJoinStatus makePipelineJoinStatus(const EtwInitializeHint& etw,
                                          const std::vector<ProbeSlice>& probes,
                                          const std::vector<HookedCall>& allCalls,
                                          const std::vector<HookedCall>& sessionCalls) {
    PipelineJoinStatus st;
    st.etwMatched = etw.present;
    st.probesPresent = !probes.empty();
    st.otherPidCalls =
        allCalls.size() >= sessionCalls.size() ? allCalls.size() - sessionCalls.size() : 0;
    for (const auto& c : sessionCalls) {
        if (!c.pump && !isPumpMethod(c.method)) ++st.sessionControlCalls;
    }
    const HookedInitializeExtract extracted = extractHookedInitializeDetailed(sessionCalls);
    st.hookedStreamCount = extracted.streamCount;
    st.primaryStreamId = extracted.primaryStreamId;
    if (extracted.hint.present)
        st.initializeSource = InitializeFieldSource::Hooked;
    else if (etw.present)
        st.initializeSource = InitializeFieldSource::Etw;
    else
        st.initializeSource = InitializeFieldSource::None;
    return st;
}

const char* initializeFieldSourceName(InitializeFieldSource src) {
    switch (src) {
        case InitializeFieldSource::None: return "none";
        case InitializeFieldSource::Etw: return "etw";
        case InitializeFieldSource::Hooked: return "hooked";
    }
    return "none";
}

}  // namespace wa
