#pragma once
#include "PipelineGraph.h"
#include "Result.h"
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace wa {

// Persist monitored Core Audio intercept rows (API Monitor-style capture save).
// JSONL one object per line; CSV is a flat export of the same fields. No PCM.
class CallLogStore {
public:
    void clear();
    void append(const std::vector<HookedCall>& calls);
    void setRecording(bool on);
    bool recording() const;
    size_t size() const;
    const std::vector<HookedCall>& entries() const { return entries_; }

    // Default dir: %LOCALAPPDATA%\WinAudio\pipeline-calls
    static std::wstring defaultDirectory();
    static std::string defaultFileStem(uint32_t pid);

    Result saveJsonl(const std::wstring& path) const;
    Result saveCsv(const std::wstring& path) const;
    // Writes JSONL under defaultDirectory(); sets outPath on success.
    Result saveDefaultJsonl(uint32_t pid, std::wstring& outPath) const;

    static std::string formatHresult(int32_t hr);
    static std::string toJsonLine(const HookedCall& c);
    static std::string toCsvHeader();
    static std::string toCsvLine(const HookedCall& c);

private:
    mutable std::mutex mtx_;
    std::vector<HookedCall> entries_;
    bool recording_ = false;
};

// Stable key for Live-session attach UI state (instance id, else pid|device|flow).
std::string liveSessionAttachKey(const LiveSessionView& row);

enum class PipelineAttachUiState : uint8_t { Detached, Attached, Failed };

const char* pipelineAttachUiStateName(PipelineAttachUiState state);

}  // namespace wa
