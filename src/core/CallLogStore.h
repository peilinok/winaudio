#pragma once
#include "HookedCall.h"
#include "CallLogFilter.h"
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

enum class CallLogStage { Sort, Match, PumpMatch, Initialize };
struct CallLogObserver {
    void* context = nullptr;
    void (*event)(void*, CallLogStage, bool begin, size_t count) = nullptr;
    void stage(CallLogStage kind, bool begin, size_t count) const {
        if (event) event(context, kind, begin, count);
    }
};

struct CallLogViewWork {
    size_t sorted = 0, matched = 0, pumpMatched = 0;
    bool controlChanged = false, queryChanged = false, pumpChanged = false;
};

// Persist monitored Core Audio intercept rows (API Monitor-style capture save).
// JSONL one object per line; CSV is a flat export of the same fields. No PCM.
class CallLogStore {
public:
    static constexpr size_t kNotFound = static_cast<size_t>(-1);
    // Borrows the current query cache; consume before the next Store mutation/view.
    class View {
    public:
        size_t size() const;
        const CallRecord* at(size_t position) const;
        size_t position(CallRecordId id) const;
        const CallLogViewWork& work() const;
        size_t indexBytes() const;
    private:
        friend class CallLogStore;
        explicit View(const CallLogStore* store) : store_(store) {}
        const CallLogStore* store_;
    };
    View view(const call_log_filter::FilterOpts& query = {}, CallLogObserver observer = {});
    const std::vector<std::string>& ifaces();
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
    void setPumpVisible(bool on);
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
    uint64_t controlRevision_ = 1, pumpRevision_ = 1;
    uint64_t sortedRevision_ = 0, matchedRevision_ = 0, matchedPumpRevision_ = 0;
    std::vector<size_t> sorted_, matched_, positions_, pumpPositions_;
    std::vector<const CallRecord*> matchedPump_;
    std::string queryText_, queryIface_;
    bool queryFailures_ = false, queryValid_ = false;
    CallLogViewWork viewWork_;
    std::vector<std::string> controlIfaces_, ifaces_;
    uint64_t ifacesControlRevision_ = 0, ifacesPumpRevision_ = 0;
};

// Stable key for Live-session attach UI state (instance id, else pid|device|flow).
std::string liveSessionAttachKey(const LiveSessionView& row);

enum class PipelineAttachUiState : uint8_t { Detached, Attached, Failed };

const char* pipelineAttachUiStateName(PipelineAttachUiState state);

}  // namespace wa
