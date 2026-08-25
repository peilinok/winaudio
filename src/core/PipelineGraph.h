#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wa {

enum class ObservationKind : uint8_t {
    Observed,
    Probed,
    Inferred,
    Skipped,
    Unknown,
};

enum class PipelineFlow : uint8_t { Capture, Render };

struct PipelineParam {
    std::string key;
    std::string value;
    ObservationKind kind = ObservationKind::Unknown;
};

struct PipelineNode {
    std::string id;
    std::string title;
    ObservationKind kind = ObservationKind::Unknown;
    std::vector<PipelineParam> params;
};

struct LiveSessionView {
    uint32_t processId = 0;
    std::string processName;
    std::string deviceId;
    std::string deviceName;
    PipelineFlow flow = PipelineFlow::Capture;
    float sessionVolume = 1.f;
    bool sessionMute = false;
    std::string state;
};

// Drop pid 0; drop hidePid when non-zero; keep one row per session instance (do not
// collapse the same PID on two devices). Sort by name, pid, flow, device.
void shapeLiveSessionList(std::vector<LiveSessionView>& rows, uint32_t hidePid);

struct ApoSlot {
    std::string role;  // "SFX" | "MFX" | "EFX"
    std::string clsid;
    std::string friendlyName;
};

struct HardwareControl {
    std::string name;
    std::string value;
};

struct EndpointSnapshot {
    bool sysFxDisabled = false;
    std::string mixFormat;
    std::string deviceFormat;
    std::string oemFormat;
    std::vector<ApoSlot> apos;
    std::vector<HardwareControl> hardware;
};

struct EtwInitializeHint {
    bool present = false;
    std::optional<std::string> category;
    std::optional<bool> raw;
    std::optional<bool> matchFormat;
    std::optional<bool> exclusive;
    std::optional<int32_t> hresult;
};

struct AdvertisedEffect {
    std::string typeName;
    bool on = true;
    bool canSetState = false;
};

struct ProbeSlice {
    std::string label;
    bool raw = false;
    std::vector<AdvertisedEffect> effects;
    std::string error;
};

const char* observationKindName(ObservationKind kind);

// Pure join of session + endpoint + optional ETW + probes. No COM, no devices.
std::vector<PipelineNode> assemblePipeline(
    const LiveSessionView& session,
    const EndpointSnapshot& endpoint,
    const EtwInitializeHint& etw,
    const std::vector<ProbeSlice>& probes);

}  // namespace wa
