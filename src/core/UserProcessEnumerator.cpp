#include "UserProcessEnumerator.h"
#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>

namespace wa {
namespace {

std::string wideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n), 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    s.resize(static_cast<size_t>(n - 1));
    return s;
}

// Token user SID bytes for a process, or empty when it cannot be read.
std::vector<unsigned char> tokenUserSid(HANDLE process) {
    std::vector<unsigned char> sid;
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return sid;
    DWORD len = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &len);
    if (len > 0) {
        std::vector<unsigned char> buf(len);
        if (GetTokenInformation(token, TokenUser, buf.data(), len, &len)) {
            PSID p = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
            if (p && IsValidSid(p)) {
                const DWORD sidLen = GetLengthSid(p);
                const auto* b = static_cast<const unsigned char*>(p);
                sid.assign(b, b + sidLen);
            }
        }
    }
    CloseHandle(token);
    return sid;
}

std::string lowerAscii(const std::string& s) {
    std::string r = s;
    for (char& ch : r) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return r;
}

}  // namespace

Result enumerateUserProcesses(std::vector<UserProcessInfo>& out) {
    out.clear();
    DWORD selfSession = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &selfSession))
        return Result::Fail(static_cast<long>(GetLastError()), "ProcessIdToSessionId(self) failed");
    const std::vector<unsigned char> selfSid = tokenUserSid(GetCurrentProcess());
    if (selfSid.empty())
        return Result::Fail(static_cast<long>(GetLastError()), "cannot read current token user");

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return Result::Fail(static_cast<long>(GetLastError()), "CreateToolhelp32Snapshot failed");

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        const DWORD pid = pe.th32ProcessID;
        if (pid == 0) continue;
        DWORD session = 0;
        if (!ProcessIdToSessionId(pid, &session) || session != selfSession) continue;
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;
        const std::vector<unsigned char> sid = tokenUserSid(h);
        CloseHandle(h);
        if (sid != selfSid) continue;
        UserProcessInfo info;
        info.pid = static_cast<uint32_t>(pid);
        info.name = wideToUtf8(pe.szExeFile);
        if (info.name.empty()) info.name = "pid-" + std::to_string(pid);
        out.push_back(std::move(info));
    }
    CloseHandle(snap);
    return Result::Ok();
}

std::vector<ProcessTreeRow> buildProcessTree(const std::vector<UserProcessInfo>& processes,
                                             const std::vector<LiveSessionView>& sessions,
                                             bool audioOnly, uint32_t hidePid) {
    std::map<uint32_t, ProcessTreeRow> byPid;
    for (int i = 0; i < static_cast<int>(sessions.size()); ++i) {
        const LiveSessionView& s = sessions[static_cast<size_t>(i)];
        if (s.processId == 0 || (hidePid != 0 && s.processId == hidePid)) continue;
        ProcessTreeRow& row = byPid[s.processId];
        row.pid = s.processId;
        if (row.name.empty()) row.name = s.processName;
        row.sessions.push_back(i);
        if (s.state == "Active") row.hasActiveSession = true;
    }
    if (!audioOnly) {
        for (const UserProcessInfo& p : processes) {
            if (p.pid == 0 || (hidePid != 0 && p.pid == hidePid)) continue;
            ProcessTreeRow& row = byPid[p.pid];
            row.pid = p.pid;
            if (row.name.empty()) row.name = p.name;
        }
    }
    std::vector<ProcessTreeRow> rows;
    rows.reserve(byPid.size());
    for (auto& kv : byPid) rows.push_back(std::move(kv.second));
    std::stable_sort(rows.begin(), rows.end(), [](const ProcessTreeRow& a, const ProcessTreeRow& b) {
        const bool aa = !a.sessions.empty();
        const bool ba = !b.sessions.empty();
        if (aa != ba) return aa;
        const std::string an = lowerAscii(a.name);
        const std::string bn = lowerAscii(b.name);
        if (an != bn) return an < bn;
        return a.pid < b.pid;
    });
    return rows;
}

}  // namespace wa