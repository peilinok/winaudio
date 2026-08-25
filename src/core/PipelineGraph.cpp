#include "PipelineGraph.h"
#include <algorithm>
#include <cstring>

namespace wa {
namespace {

PipelineParam param(std::string key, std::string value, ObservationKind kind) {
    return PipelineParam{std::move(key), std::move(value), kind};
}

void addAposForRole(PipelineNode& node, const EndpointSnapshot& endpoint, const char* role) {
    int n = 0;
    for (const auto& apo : endpoint.apos) {
        if (apo.role != role) continue;
        ++n;
        const std::string label = apo.friendlyName.empty() ? apo.clsid : apo.friendlyName;
        node.params.push_back(param(role, label + " (" + apo.clsid + ")", ObservationKind::Observed));
    }
    if (n == 0) {
        node.params.push_back(param(role, "(none registered)", ObservationKind::Observed));
    }
}

void addProbeEffects(PipelineNode& node, const std::vector<ProbeSlice>& probes) {
    if (probes.empty())
        return;
    for (const auto& slice : probes) {
        if (!slice.error.empty()) {
            node.params.push_back(param(slice.label, slice.error, ObservationKind::Unknown));
            continue;
        }
        if (slice.effects.empty()) {
            node.params.push_back(param(slice.label, "(no advertised effects)", ObservationKind::Probed));
            continue;
        }
        for (const auto& fx : slice.effects) {
            std::string v = fx.typeName;
            v += fx.on ? " ON" : " OFF";
            if (fx.canSetState) v += " (settable)";
            node.params.push_back(param(slice.label, std::move(v), ObservationKind::Probed));
        }
    }
}

PipelineNode hardwareNode(const EndpointSnapshot& endpoint) {
    PipelineNode n{"hardware", "Hardware", ObservationKind::Inferred, {}};
    if (!endpoint.deviceFormat.empty()) {
        n.params.push_back(param("device format", endpoint.deviceFormat, ObservationKind::Observed));
        n.kind = ObservationKind::Observed;
    }
    if (!endpoint.oemFormat.empty()) {
        n.params.push_back(param("OEM format", endpoint.oemFormat, ObservationKind::Observed));
        n.kind = ObservationKind::Observed;
    }
    for (const auto& c : endpoint.hardware) {
        n.params.push_back(param(c.name.c_str(), c.value, ObservationKind::Observed));
        n.kind = ObservationKind::Observed;
    }
    if (n.params.empty()) {
        n.params.push_back(param("topology", "no readable controls", ObservationKind::Unknown));
    }
    return n;
}

PipelineNode driverNode() {
    return {"driver", "Driver / KS", ObservationKind::Inferred,
            {param("note", "kernel path not dumped in v1", ObservationKind::Inferred)}};
}

PipelineNode efxNode(const EndpointSnapshot& endpoint, const std::vector<ProbeSlice>& probes,
                     bool exclusive) {
    PipelineNode n{"efx", "EFX (endpoint)", ObservationKind::Observed, {}};
    addAposForRole(n, endpoint, "EFX");
    if (exclusive) {
        n.kind = ObservationKind::Unknown;
        n.params.push_back(param("exclusive", "software EFX usually not on this path",
                                 ObservationKind::Inferred));
    }
    addProbeEffects(n, probes);
    return n;
}

PipelineNode mfxNode(const EndpointSnapshot& endpoint, const EtwInitializeHint& etw,
                     const std::vector<ProbeSlice>& probes, bool skipSysFx) {
    PipelineNode n{"mfx", "MFX (mode)", ObservationKind::Observed, {}};
    if (skipSysFx) {
        n.kind = ObservationKind::Skipped;
        addAposForRole(n, endpoint, "MFX");
        n.params.push_back(param("SysFx", "disabled", ObservationKind::Observed));
        return n;
    }
    addAposForRole(n, endpoint, "MFX");
    if (etw.present && etw.category) {
        n.params.push_back(param("category", *etw.category, ObservationKind::Observed));
    } else {
        n.params.push_back(param("category", "unknown", ObservationKind::Unknown));
    }
    if (etw.present && etw.raw && *etw.raw) {
        n.params.push_back(param("RAW", "SFX skipped; MFX may still load", ObservationKind::Inferred));
        n.kind = ObservationKind::Unknown;
    }
    addProbeEffects(n, probes);
    return n;
}

PipelineNode sfxNode(const EndpointSnapshot& endpoint, const EtwInitializeHint& etw,
                     const std::vector<ProbeSlice>& probes, bool skipSysFx) {
    PipelineNode n{"sfx", "SFX (stream)", ObservationKind::Observed, {}};
    if (skipSysFx) {
        n.kind = ObservationKind::Skipped;
        addAposForRole(n, endpoint, "SFX");
        n.params.push_back(param("SysFx", "disabled", ObservationKind::Observed));
        return n;
    }
    if (etw.present && etw.raw && *etw.raw) {
        n.kind = ObservationKind::Skipped;
        n.params.push_back(param("RAW", "SFX not used", ObservationKind::Observed));
        addAposForRole(n, endpoint, "SFX");
        return n;
    }
    addAposForRole(n, endpoint, "SFX");
    if (!etw.present || !etw.category) {
        n.params.push_back(param("instance", "per-stream; not the watched session",
                                 ObservationKind::Unknown));
    }
    addProbeEffects(n, probes);
    return n;
}

PipelineNode srcNode(const EndpointSnapshot& endpoint, const EtwInitializeHint& etw) {
    PipelineNode n{"src", "Engine SRC / mix format", ObservationKind::Observed, {}};
    if (!endpoint.mixFormat.empty()) {
        n.params.push_back(param("mix format", endpoint.mixFormat, ObservationKind::Observed));
    } else {
        n.params.push_back(param("mix format", "unavailable", ObservationKind::Unknown));
        n.kind = ObservationKind::Unknown;
    }
    n.params.push_back(param("note", "mix format is the engine, not the app stream format",
                             ObservationKind::Inferred));
    if (etw.present && etw.matchFormat) {
        n.params.push_back(param("MATCH_FORMAT", *etw.matchFormat ? "requested" : "not requested",
                                 ObservationKind::Observed));
    }
    return n;
}

PipelineNode sessionNode(const LiveSessionView& session) {
    PipelineNode n{"session", "Session", ObservationKind::Observed, {}};
    n.params.push_back(param("pid", std::to_string(session.processId), ObservationKind::Observed));
    n.params.push_back(param("volume", std::to_string(session.sessionVolume), ObservationKind::Observed));
    n.params.push_back(param("mute", session.sessionMute ? "true" : "false", ObservationKind::Observed));
    n.params.push_back(param("device", session.deviceName.empty() ? session.deviceId : session.deviceName,
                             ObservationKind::Observed));
    return n;
}

PipelineNode appNode(const LiveSessionView& session) {
    PipelineNode n{"app", "App", ObservationKind::Observed, {}};
    const std::string name = session.processName.empty()
                                 ? ("pid-" + std::to_string(session.processId))
                                 : session.processName;
    n.params.push_back(param("process", name, ObservationKind::Observed));
    n.params.push_back(param("flow", session.flow == PipelineFlow::Capture ? "capture" : "render",
                             ObservationKind::Observed));
    return n;
}

void appendEngine(std::vector<PipelineNode>& out, const EndpointSnapshot& endpoint,
                  const EtwInitializeHint& etw, const std::vector<ProbeSlice>& probes,
                  bool capture) {
    const bool exclusive = etw.present && etw.exclusive && *etw.exclusive;
    const bool skipSysFx = endpoint.sysFxDisabled || exclusive;

    auto pushSfxMfxSrc = [&]() {
        if (exclusive) {
            PipelineNode skipped{"engine", "Audio engine (Shared)", ObservationKind::Skipped,
                                 {param("exclusive", "bypassed", ObservationKind::Observed)}};
            if (etw.hresult) {
                skipped.params.push_back(param("Initialize HRESULT", std::to_string(*etw.hresult),
                                               ObservationKind::Observed));
            }
            out.push_back(std::move(skipped));
            return;
        }
        if (capture) {
            out.push_back(mfxNode(endpoint, etw, probes, skipSysFx));
            out.push_back(sfxNode(endpoint, etw, probes, skipSysFx));
            out.push_back(srcNode(endpoint, etw));
        } else {
            out.push_back(srcNode(endpoint, etw));
            out.push_back(sfxNode(endpoint, etw, probes, skipSysFx));
            out.push_back(mfxNode(endpoint, etw, probes, skipSysFx));
        }
    };

    if (capture) {
        out.push_back(efxNode(endpoint, {}, exclusive));
        pushSfxMfxSrc();
    } else {
        pushSfxMfxSrc();
        out.push_back(efxNode(endpoint, {}, exclusive));
    }
}

}  // namespace

const char* observationKindName(ObservationKind kind) {
    switch (kind) {
        case ObservationKind::Observed: return "Observed";
        case ObservationKind::Probed:   return "Probed";
        case ObservationKind::Inferred: return "Inferred";
        case ObservationKind::Skipped:  return "Skipped";
        case ObservationKind::Unknown:  return "Unknown";
    }
    return "Unknown";
}

std::vector<PipelineNode> assemblePipeline(
    const LiveSessionView& session,
    const EndpointSnapshot& endpoint,
    const EtwInitializeHint& etw,
    const std::vector<ProbeSlice>& probes) {
    std::vector<PipelineNode> out;
    const bool capture = session.flow == PipelineFlow::Capture;

    if (capture) {
        out.push_back(hardwareNode(endpoint));
        out.push_back(driverNode());
        appendEngine(out, endpoint, etw, probes, true);
        out.push_back(sessionNode(session));
        out.push_back(appNode(session));
    } else {
        out.push_back(appNode(session));
        out.push_back(sessionNode(session));
        appendEngine(out, endpoint, etw, probes, false);
        out.push_back(driverNode());
        out.push_back(hardwareNode(endpoint));
    }

    if (etw.present && etw.hresult) {
        // Attach HRESULT to the session node so Initialize result is visible even on Exclusive.
        for (auto& n : out) {
            if (n.id == "session") {
                n.params.push_back(param("Initialize HRESULT", std::to_string(*etw.hresult),
                                         ObservationKind::Observed));
                break;
            }
        }
    }
    return out;
}

void shapeLiveSessionList(std::vector<LiveSessionView>& rows, uint32_t hidePid) {
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [hidePid](const LiveSessionView& row) {
                                  if (row.processId == 0) return true;
                                  return hidePid != 0 && row.processId == hidePid;
                              }),
               rows.end());
    std::sort(rows.begin(), rows.end(), [](const LiveSessionView& a, const LiveSessionView& b) {
        const int nameCmp = _stricmp(a.processName.c_str(), b.processName.c_str());
        if (nameCmp != 0) return nameCmp < 0;
        if (a.processId != b.processId) return a.processId < b.processId;
        if (a.flow != b.flow) return static_cast<uint8_t>(a.flow) < static_cast<uint8_t>(b.flow);
        const int devCmp = _stricmp(a.deviceName.c_str(), b.deviceName.c_str());
        if (devCmp != 0) return devCmp < 0;
        return a.deviceId < b.deviceId;
    });
}

}  // namespace wa
