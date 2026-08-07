#pragma once
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

struct DeviceInfo {
    DeviceId     id;
    std::wstring name;
    DataFlow     flow = DataFlow::Render;
    bool         isDefault = false;
    AudioFormat  mixFormat{};       // shared-mode mix format
    AudioFormat  deviceFormat{}; bool hasDeviceFormat = false; // PKEY_AudioEngine_DeviceFormat
    AudioFormat  oemFormat{};    bool hasOemFormat    = false; // PKEY_AudioEngine_OEMFormat
};

struct BackendStats {
    AudioFormat actualFormat{};
    uint32_t    bufferFrames = 0;
    uint64_t    overruns = 0;
    uint64_t    underruns = 0;
    uint64_t    idleSilenceFrames = 0;
    uint64_t    silentPacketFrames = 0;
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
};

} // namespace wa
