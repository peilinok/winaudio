#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "AudioFormat.h"
#include "Result.h"
#include "StreamParams.h"

namespace wa {

class RingBuffer;

enum class DataFlow { Capture, Render };
using DeviceId = std::wstring;     // MMDevice id; empty string = default endpoint

enum class CaptureSourceKind {
    Endpoint,
    SystemLoopback,
    ApplicationLoopback,
};

// ApplicationLoopback only; Endpoint / SystemLoopback ignore this field.
enum class ProcessLoopbackMode {
    IncludeTree,
    ExcludeTree,
};

inline const char* processLoopbackModeName(ProcessLoopbackMode mode) {
    return mode == ProcessLoopbackMode::ExcludeTree ? "ExcludeTree" : "IncludeTree";
}

struct CaptureSource {
    CaptureSourceKind kind = CaptureSourceKind::Endpoint;
    DeviceId deviceId; // Endpoint: capture endpoint; SystemLoopback: render endpoint
    uint32_t processId = 0; // ApplicationLoopback target PID
    ProcessLoopbackMode processLoopbackMode = ProcessLoopbackMode::IncludeTree;
};

struct LoopbackOptions {
    bool silentRender = true;
};

// Endpoint system-effects master switch (PKEY_AudioEndpoint_Disable_SysFx).
// Unknown = property missing or unreadable; not the same as Disabled.
enum class EnhancementsState : uint8_t {
    Unknown = 0,
    Enabled,
    Disabled,
};

inline const char* enhancementsStateName(EnhancementsState s) {
    switch (s) {
    case EnhancementsState::Enabled:  return "on";
    case EnhancementsState::Disabled: return "off";
    default:                          return "unknown";
    }
}

// Map ENDPOINT_SYSFX_* (VT_UI4 ulVal). Call only when the property was present.
inline EnhancementsState enhancementsStateFromSysFxUlVal(uint32_t ulVal) {
    return ulVal == 0 ? EnhancementsState::Enabled : EnhancementsState::Disabled;
}

// Discoverable enhancement descriptor (APO slot CLSID, etc.). Empty list is normal
// when the endpoint publishes no FX property keys; not an error.
struct EnhancementOption {
    std::string id;    // stable key, e.g. "SFX" | "MFX" | "EFX"
    std::string name;  // short English label
    std::string value; // e.g. CLSID string
};

struct DeviceInfo {
    DeviceId     id;
    std::wstring name;
    DataFlow     flow = DataFlow::Render;
    bool         isDefault = false;
    AudioFormat  mixFormat{};       // shared-mode mix format
    AudioFormat  deviceFormat{}; bool hasDeviceFormat = false; // PKEY_AudioEngine_DeviceFormat
    AudioFormat  oemFormat{};    bool hasOemFormat    = false; // PKEY_AudioEngine_OEMFormat
    EnhancementsState enhancements = EnhancementsState::Unknown;
    std::vector<EnhancementOption> enhancementOptions;
};

struct BackendStats {
    AudioFormat actualFormat{};
    uint32_t    bufferFrames = 0;
    uint64_t    overruns = 0;
    uint64_t    underruns = 0;
    uint64_t    idleSilenceFrames = 0;
    uint64_t    silentPacketFrames = 0;
    long        runtimeError = 0;   // first worker-thread HRESULT failure; 0 = healthy
};

class IAudioBackend {
public:
    virtual ~IAudioBackend() = default;
    virtual Result open(const DeviceId& id, const AudioFormat& fmt, RingBuffer* ring,
                        const StreamParams& params) = 0;
    virtual Result start() = 0;
    virtual void   stop() = 0;
    virtual void   close() = 0;
    virtual BackendStats stats() const = 0;

    // Auto-reset event a monitor pump can wait on; signaled after each capture ring write.
    // nullptr if the backend provides no such signal (default).
    virtual void* dataReadyEvent() const { return nullptr; }

    // Runtime fault observed by a pump; default is healthy. Does not stop the peer stream.
    virtual bool inError() const { return false; }
};

} // namespace wa
