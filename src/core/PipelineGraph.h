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
    // Empty when the enumerator cannot read IAudioSessionControl2's instance id.
    std::string sessionInstanceId;
    float sessionVolume = 1.f;
    bool sessionMute = false;
    std::string state;
};

// Drop pid 0; drop hidePid when non-zero. Do not collapse two rows that share a
// PID, including two sessions on one device and flow. Sort by name, pid, flow, device.
void shapeLiveSessionList(std::vector<LiveSessionView>& rows, uint32_t hidePid);

// Index of the same Live session after a list rebuild, or -1.
// A non-empty session instance id matches only that id. An empty id matches
// process id + device id + flow (the first such row).
int restoreLiveSessionSelection(const std::vector<LiveSessionView>& rows,
                                const LiveSessionView& selected);

// Volume and/or mute and/or state for one Live session row. Identity uses the
// same rule as restoreLiveSessionSelection. Channel volume, display name, and
// icon are not fields of this patch.
struct LiveSessionCellPatch {
    std::string sessionInstanceId;
    uint32_t processId = 0;
    std::string deviceId;
    PipelineFlow flow = PipelineFlow::Capture;
    bool hasVolume = false;
    float volume = 0.f;
    bool hasMute = false;
    bool mute = false;
    bool hasState = false;
    std::string state;
};

// Writes the set fields onto the one matching row. Returns that index, or -1
// when nothing is set or no row matches. Other rows are left unchanged.
int applyLiveSessionCellPatch(std::vector<LiveSessionView>& rows,
                              const LiveSessionCellPatch& patch);

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
    std::optional<std::string> format;
};

struct HookedCall {
    uint32_t streamId = 0;
    int64_t timeMs = 0;
    std::string iface;
    std::string method;
    std::string args;
    int32_t hresult = 0;
    bool pump = false;
    bool xrun = false;
    std::optional<std::string> category;
    std::optional<bool> raw;
    std::optional<bool> matchFormat;
    std::optional<bool> exclusive;
    std::optional<std::string> format;
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

// Pure join of session + endpoint + optional ETW + probes + hooked calls.
// Hooked Initialize fields override matching ETW fields and stay Observed.
// No COM, no devices.
std::vector<PipelineNode> assemblePipeline(
    const LiveSessionView& session,
    const EndpointSnapshot& endpoint,
    const EtwInitializeHint& etw,
    const std::vector<ProbeSlice>& probes,
    const std::vector<HookedCall>& hooked = {});

}  // namespace wa
