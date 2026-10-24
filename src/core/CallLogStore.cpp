#include "CallLogStore.h"
#include "HookedCall.h"
#include "Log.h"
#include <cstdio>
#include <algorithm>
#include <fstream>
#include <sstream>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

namespace wa {
namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string csvEscape(const std::string& s) {
    bool need = false;
    for (char c : s) {
        if (c == '"' || c == ',' || c == '\n' || c == '\r') {
            need = true;
            break;
        }
    }
    if (!need) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

Result writeRecords(const std::wstring& path, const CallLogExport& snapshot, bool csv) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)),
                            "CallLogStore: open failed");
    bool ok = true;
    const auto writeLine = [&](const std::string& line) {
        return fwrite(line.data(), 1, line.size(), f) == line.size() && fputc('\n', f) != EOF;
    };
    if (csv) ok = writeLine(CallLogStore::toCsvHeader());
    for (const auto& record : snapshot.records) {
        if (!ok) break;
        ok = writeLine(csv ? CallLogStore::toCsvLine(record->call) : CallLogStore::toJsonLine(*record));
    }
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok ? Result::Ok() : Result::Fail(static_cast<long>(E_FAIL), "CallLogStore: write/close failed");
}

bool ensureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    return CreateDirectoryW(dir.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace

void CallLogStore::clear() { beginRound(); }

void CallLogStore::beginRound() {
    entries_.clear();
    attaches_.clear();
    latestAttach_.clear();
    pidCounts_.clear();
    pump_.clear();
    ++round_;
    range_ = recording_ ? RecordingRange::Following : RecordingRange::Never;
    frozenCount_ = 0;
}

uint64_t CallLogStore::registerAttach(uint32_t pid) {
    if (const auto active = activeAttach(pid)) return active;
    attaches_.push_back({pid, true, {}, {}});
    const uint64_t id = attaches_.size();
    latestAttach_[pid] = id;
    return id;
}

void CallLogStore::endAttach(uint64_t attach) {
    if (attach == 0 || attach > attaches_.size()) return;
    auto& segment = attaches_[static_cast<size_t>(attach - 1)];
    segment.active = false;
    segment.pump.clear();
    rebuildPump();
}

const std::vector<CallRecordRef>& CallLogStore::pumpEntries() const {
    static const std::vector<CallRecordRef> empty;
    return pumpVisible_ ? pump_ : empty;
}

void CallLogStore::rebuildPump() {
    pump_.clear();
    for (const auto& segment : attaches_)
        if (segment.active) pump_.insert(pump_.end(), segment.pump.begin(), segment.pump.end());
    std::sort(pump_.begin(), pump_.end(), [](const CallRecordRef& a, const CallRecordRef& b) {
        if (a->call.timeMs != b->call.timeMs) return a->call.timeMs < b->call.timeMs;
        if (a->id.attach != b->id.attach) return a->id.attach < b->id.attach;
        return a->id.sequence < b->id.sequence;
    });
    if (pump_.size() > kDefaultPumpRingCap)
        pump_.erase(pump_.begin(), pump_.end() - static_cast<std::ptrdiff_t>(kDefaultPumpRingCap));
}

void CallLogStore::updatePump(uint64_t attach, const std::vector<PumpCall>& window) {
    if (attach == 0 || attach > attaches_.size()) return;
    auto& segment = attaches_[static_cast<size_t>(attach - 1)];
    if (!segment.active) return;
    bool unchanged = segment.pump.size() == window.size();
    for (size_t i = 0; unchanged && i < window.size(); ++i)
        unchanged = segment.pump[i]->id.sequence == window[i].sequence;
    if (unchanged) return;
    std::vector<CallRecordRef> next;
    next.reserve(window.size());
    for (const auto& observed : window) {
        const auto old = std::find_if(segment.pump.begin(), segment.pump.end(), [&](const CallRecordRef& ref) {
            return ref->id.sequence == observed.sequence;
        });
        if (old != segment.pump.end()) { next.push_back(*old); continue; }
        auto call = observed.call;
        call.pid = segment.pid;
        call.pump = true;
        call.args = sanitizeHookedArgs(std::move(call.args));
        CallRecord record{{round_, attach, observed.sequence, CallLane::Pump}, std::move(call)};
        next.push_back(std::make_shared<const CallRecord>(std::move(record)));
    }
    segment.pump = std::move(next);
    rebuildPump();
}

uint64_t CallLogStore::latestAttach(uint32_t pid) const {
    const auto it = latestAttach_.find(pid);
    return it == latestAttach_.end() ? 0 : it->second;
}

uint64_t CallLogStore::activeAttach(uint32_t pid) const {
    const auto id = latestAttach(pid);
    return id && attaches_[static_cast<size_t>(id - 1)].active ? id : 0;
}

void CallLogStore::append(uint64_t attach, HookedCall call) {
    if (attach == 0 || attach > attaches_.size()) return;
    auto& segment = attaches_[static_cast<size_t>(attach - 1)];
    if (!segment.active || call.pump || isPumpMethod(call.method)) return;
    if (segment.pid != 0) call.pid = segment.pid;
    call.args = sanitizeHookedArgs(std::move(call.args));
    CallRecord record{{round_, attach, entries_.size() + 1, CallLane::Control}, std::move(call)};
    auto ref = std::make_shared<const CallRecord>(std::move(record));
    ++pidCounts_[ref->call.pid];
    segment.records.push_back(ref);
    entries_.push_back(std::move(ref));
}

void CallLogStore::append(uint64_t attach, std::vector<HookedCall> calls) {
    for (auto& call : calls) append(attach, std::move(call));
}

void CallLogStore::append(std::vector<HookedCall> calls) {
    for (auto& call : calls) append(registerAttach(call.pid), std::move(call));
}

const CallRecord* CallLogStore::find(CallRecordId id) const {
    if (id.round == round_ && id.lane == CallLane::Pump) {
        for (const auto& ref : pumpEntries()) if (ref->id == id) return ref.get();
        return nullptr;
    }
    if (id.round != round_ || id.lane != CallLane::Control || id.sequence == 0 ||
        id.sequence > entries_.size()) return nullptr;
    const auto& ref = entries_[static_cast<size_t>(id.sequence - 1)];
    return ref->id == id ? ref.get() : nullptr;
}

size_t CallLogStore::pidCallCount(uint32_t pid) const {
    const auto it = pidCounts_.find(pid);
    return it == pidCounts_.end() ? 0 : it->second;
}

std::vector<const HookedCall*> CallLogStore::sessionRecords(uint32_t pid) const {
    std::vector<const CallRecord*> records;
    const auto appendSegment = [&](uint64_t id) {
        if (id == 0) return;
        for (const auto& ref : attaches_[static_cast<size_t>(id - 1)].records) records.push_back(ref.get());
    };
    appendSegment(latestAttach(pid));
    if (pid != 0) appendSegment(latestAttach(0));
    std::sort(records.begin(), records.end(), [](const CallRecord* a, const CallRecord* b) {
        if (a->call.timeMs != b->call.timeMs) return a->call.timeMs < b->call.timeMs;
        return a->id.sequence < b->id.sequence;
    });
    std::vector<const HookedCall*> out;
    out.reserve(records.size());
    for (const auto* record : records) out.push_back(&record->call);
    return out;
}

void CallLogStore::setRecording(bool on) {
    if (on == recording_) return;
    recording_ = on;
    range_ = on ? RecordingRange::Following : RecordingRange::Frozen;
    if (!on) frozenCount_ = entries_.size();
}

bool CallLogStore::recording() const {
    return recording_;
}

size_t CallLogStore::size() const {
    return entries_.size();
}

CallLogExport CallLogStore::freezeExport() const {
    const size_t count = exportCount();
    CallLogExport out{round_, count, {}};
    out.records.assign(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(count));
    return out;
}

size_t CallLogStore::exportCount() const {
    return range_ == RecordingRange::Frozen ? frozenCount_ : entries_.size();
}

std::wstring CallLogStore::defaultDirectory() {
    wchar_t* path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) || !path)
        return {};
    std::wstring dir = path;
    CoTaskMemFree(path);
    dir += L"\\WinAudio\\pipeline-calls";
    return dir;
}

std::string CallLogStore::defaultFileStem(uint32_t pid) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[96];
    sprintf_s(buf, "calls-pid%u-%04u%02u%02u-%02u%02u%02u", pid, st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string CallLogStore::exportFileStem(const CallLogExport& snapshot) {
    const uint32_t pid = snapshot.records.empty() ? 0 : snapshot.records.front()->call.pid;
    std::string stem = defaultFileStem(pid);
    for (const auto& record : snapshot.records) {
        if (record->call.pid == pid) continue;
        stem.replace(6, stem.find('-', 6) - 6, "multi");
        break;
    }
    return stem;
}

std::string CallLogStore::formatHresult(int32_t hr) {
    return wa::log::hrName(hr);
}

std::string CallLogStore::formatHresultHex(int32_t hr) {
    char buf[16];
    sprintf_s(buf, "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

std::string CallLogStore::toJsonLine(const HookedCall& c) {
    std::ostringstream oss;
    oss << "{\"timeMs\":" << c.timeMs << ",\"pid\":" << c.pid << ",\"iface\":\"" << jsonEscape(c.iface) << "\",\"method\":\""
        << jsonEscape(c.method) << "\",\"args\":\"" << jsonEscape(c.args) << "\",\"hr\":\""
        << jsonEscape(formatHresult(c.hresult)) << "\",\"hrHex\":\"" << formatHresultHex(c.hresult)
        << "\",\"hresult\":" << c.hresult << ",\"streamId\":" << c.streamId
        << ",\"pump\":" << (c.pump ? "true" : "false") << ",\"xrun\":" << (c.xrun ? "true" : "false");
    if (c.category) oss << ",\"category\":\"" << jsonEscape(*c.category) << "\"";
    if (c.format) oss << ",\"format\":\"" << jsonEscape(*c.format) << "\"";
    if (c.exclusive) oss << ",\"exclusive\":" << (*c.exclusive ? "true" : "false");
    if (c.raw) oss << ",\"raw\":" << (*c.raw ? "true" : "false");
    if (c.matchFormat) oss << ",\"matchFormat\":" << (*c.matchFormat ? "true" : "false");
    if (!c.parameters.empty()) {
        oss << ",\"parameters\":[";
        for (size_t i = 0; i < c.parameters.size(); ++i) {
            const auto& a = c.parameters[i];
            if (i) oss << ',';
            oss << "{\"depth\":" << static_cast<unsigned>(a.depth) << ",\"dir\":\""
                << callArgDirName(a.dir) << "\",\"type\":\"" << jsonEscape(a.type) << "\",\"name\":\""
                << jsonEscape(a.name) << "\",\"pre\":\"" << jsonEscape(a.pre) << "\",\"post\":\""
                << jsonEscape(a.post) << "\"}";
        }
        oss << ']';
    }
    oss << "}";
    return oss.str();
}

std::string CallLogStore::toCsvHeader() {
    return "timeMs,pid,iface,method,args,hr,hresult,streamId,pump,xrun";
}

std::string CallLogStore::toJsonLine(const CallRecord& record) {
    std::string line = toJsonLine(record.call);
    line.pop_back();
    line += ",\"roundId\":" + std::to_string(record.id.round) +
        ",\"attachId\":" + std::to_string(record.id.attach) +
        ",\"lane\":\"control\",\"sequence\":" + std::to_string(record.id.sequence) + "}";
    return line;
}

std::string CallLogStore::toCsvLine(const HookedCall& c) {
    std::ostringstream oss;
    oss << c.timeMs << ',' << c.pid << ',' << csvEscape(c.iface) << ',' << csvEscape(c.method) << ','
        << csvEscape(c.args) << ',' << formatHresult(c.hresult) << ',' << c.hresult << ','
        << c.streamId << ',' << (c.pump ? 1 : 0) << ',' << (c.xrun ? 1 : 0);
    return oss.str();
}

Result CallLogStore::saveJsonl(const std::wstring& path) const {
    return writeRecords(path, freezeExport(), false);
}

Result CallLogStore::saveCsv(const std::wstring& path) const {
    return writeRecords(path, freezeExport(), true);
}

Result CallLogStore::saveDefaultJsonl(std::wstring& outPath) const {
    const std::wstring dir = defaultDirectory();
    if (dir.empty() || !ensureDir(dir))
        return Result::Fail(static_cast<long>(E_FAIL), "CallLogStore: cannot create directory");
    const auto snapshot = freezeExport();
    const std::string stem = exportFileStem(snapshot);
    outPath = dir + L"\\" + std::wstring(stem.begin(), stem.end()) + L".jsonl";
    return writeRecords(outPath, snapshot, false);
}

std::string liveSessionAttachKey(const LiveSessionView& row) {
    if (!row.sessionInstanceId.empty()) return row.sessionInstanceId;
    return std::to_string(row.processId) + "|" + row.deviceId + "|" +
           (row.flow == PipelineFlow::Render ? "R" : "C");
}

const char* pipelineAttachUiStateName(PipelineAttachUiState state) {
    switch (state) {
        case PipelineAttachUiState::Attached: return "Attached";
        case PipelineAttachUiState::Failed: return "Failed";
        case PipelineAttachUiState::Detached:
        default: return "Detached";
    }
}

}  // namespace wa
