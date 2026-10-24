#pragma once
#include "HookedCall.h"
#include "Result.h"
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wa {

enum class CallLane : uint8_t { Control, Pump };

struct CallRecordId {
    uint64_t round = 0, attach = 0, sequence = 0;
    CallLane lane = CallLane::Control;
    explicit operator bool() const { return round != 0 && sequence != 0; }
};

inline bool operator==(const CallRecordId& a, const CallRecordId& b) {
    return a.round == b.round && a.attach == b.attach && a.sequence == b.sequence && a.lane == b.lane;
}
inline bool operator!=(const CallRecordId& a, const CallRecordId& b) { return !(a == b); }

struct CallRecord {
    CallRecordId id;
    HookedCall call;
};
using CallRecordRef = std::shared_ptr<const CallRecord>;

struct CallLogExport {
    uint64_t round = 0, upperSequence = 0;
    std::vector<CallRecordRef> records;
};

// Persist monitored Core Audio intercept rows (API Monitor-style capture save).
// JSONL one object per line; CSV is a flat export of the same fields. No PCM.
class CallLogStore {
public:
    void clear();
    void beginRound();
    uint64_t round() const { return round_; }
    bool hasSources() const { return !attaches_.empty(); }
    uint64_t registerAttach(uint32_t pid);
    void endAttach(uint64_t attach);
    uint64_t activeAttach(uint32_t pid) const;
    uint64_t latestAttach(uint32_t pid) const;
    void append(uint64_t attach, std::vector<HookedCall> calls);
    void append(uint64_t attach, HookedCall call);
    void append(std::vector<HookedCall> calls);
    const CallRecord* find(CallRecordId id) const;
    std::vector<const HookedCall*> sessionRecords(uint32_t pid) const;
    size_t pidCallCount(uint32_t pid) const;
    void updatePump(uint64_t attach, const std::vector<PumpCall>& window);
    void setPumpVisible(bool on) { pumpVisible_ = on; }
    const std::vector<CallRecordRef>& pumpEntries() const;
    void setRecording(bool on);
    bool recording() const;
    size_t size() const;
    const std::vector<CallRecordRef>& entries() const { return entries_; }
    CallLogExport freezeExport() const;
    size_t exportCount() const;

    // Default dir: %LOCALAPPDATA%\WinAudio\pipeline-calls
    static std::wstring defaultDirectory();
    static std::string defaultFileStem(uint32_t pid);

    Result saveJsonl(const std::wstring& path) const;
    Result saveCsv(const std::wstring& path) const;
    // Writes JSONL under defaultDirectory(); sets outPath on success.
    Result saveDefaultJsonl(std::wstring& outPath) const;

    static std::string formatHresult(int32_t hr);
    static std::string formatHresultHex(int32_t hr);
    static std::string toJsonLine(const HookedCall& c);
    static std::string toJsonLine(const CallRecord& record);
    static std::string exportFileStem(const CallLogExport& snapshot);
    static std::string toCsvHeader();
    static std::string toCsvLine(const HookedCall& c);

private:
    // Accessed by the GUI/control thread. Export workers only own frozen refs.
    std::vector<CallRecordRef> entries_;
    struct AttachSegment {
        uint32_t pid = 0;
        bool active = true;
        std::vector<CallRecordRef> records;
        std::vector<CallRecordRef> pump;
    };
    std::vector<AttachSegment> attaches_;
    std::unordered_map<uint32_t, uint64_t> latestAttach_;
    std::unordered_map<uint32_t, size_t> pidCounts_;
    std::vector<CallRecordRef> pump_;
    bool pumpVisible_ = false;
    void rebuildPump();
    uint64_t round_ = 1;
    enum class RecordingRange { Never, Following, Frozen };
    RecordingRange range_ = RecordingRange::Never;
    size_t frozenCount_ = 0;
    bool recording_ = false;
};

// Stable key for Live-session attach UI state (instance id, else pid|device|flow).
std::string liveSessionAttachKey(const LiveSessionView& row);

enum class PipelineAttachUiState : uint8_t { Detached, Attached, Failed };

const char* pipelineAttachUiStateName(PipelineAttachUiState state);

}  // namespace wa
