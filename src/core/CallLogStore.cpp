#include "CallLogStore.h"
#include "HookedCall.h"
#include "Log.h"
#include <cstdio>
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

Result writeTextFile(const std::wstring& path, const std::string& body) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)),
                            "CallLogStore: open failed");
    const size_t n = fwrite(body.data(), 1, body.size(), f);
    fclose(f);
    if (n != body.size())
        return Result::Fail(static_cast<long>(E_FAIL), "CallLogStore: write failed");
    return Result::Ok();
}

bool ensureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    return CreateDirectoryW(dir.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace

void CallLogStore::clear() {
    std::lock_guard<std::mutex> lk(mtx_);
    entries_.clear();
}

void CallLogStore::append(const std::vector<HookedCall>& calls) {
    if (calls.empty()) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (!recording_) return;
    entries_.insert(entries_.end(), calls.begin(), calls.end());
}

void CallLogStore::setRecording(bool on) {
    std::lock_guard<std::mutex> lk(mtx_);
    recording_ = on;
}

bool CallLogStore::recording() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return recording_;
}

size_t CallLogStore::size() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return entries_.size();
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

std::string CallLogStore::toCsvLine(const HookedCall& c) {
    std::ostringstream oss;
    oss << c.timeMs << ',' << c.pid << ',' << csvEscape(c.iface) << ',' << csvEscape(c.method) << ','
        << csvEscape(c.args) << ',' << formatHresult(c.hresult) << ',' << c.hresult << ','
        << c.streamId << ',' << (c.pump ? 1 : 0) << ',' << (c.xrun ? 1 : 0);
    return oss.str();
}

Result CallLogStore::saveJsonl(const std::wstring& path) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string body;
    body.reserve(entries_.size() * 128 + 16);
    for (const auto& c : entries_) {
        body += toJsonLine(c);
        body.push_back('\n');
    }
    return writeTextFile(path, body);
}

Result CallLogStore::saveCsv(const std::wstring& path) const {
    std::lock_guard<std::mutex> lk(mtx_);
    std::string body = toCsvHeader();
    body.push_back('\n');
    for (const auto& c : entries_) {
        body += toCsvLine(c);
        body.push_back('\n');
    }
    return writeTextFile(path, body);
}

Result CallLogStore::saveDefaultJsonl(uint32_t pid, std::wstring& outPath) const {
    const std::wstring dir = defaultDirectory();
    if (dir.empty() || !ensureDir(dir))
        return Result::Fail(static_cast<long>(E_FAIL), "CallLogStore: cannot create directory");
    const std::string stem = defaultFileStem(pid);
    outPath = dir + L"\\" + std::wstring(stem.begin(), stem.end()) + L".jsonl";
    return saveJsonl(outPath);
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
