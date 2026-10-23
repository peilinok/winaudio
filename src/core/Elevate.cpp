#include "Elevate.h"
#include "Log.h"
#include "ComUtil.h"
#include <shellapi.h>

namespace wa {
namespace {

std::string narrowForLog(const std::wstring& w) {
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s += (c >= 32 && c < 127) ? static_cast<char>(c) : '?';
    return s;
}

}  // namespace

bool processIsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        WA_LOG(wa::log::Level::Debug, "Elevate", "OpenProcessToken", "",
               "err=" + std::to_string(GetLastError()));
        return false;
    }
    TOKEN_ELEVATION elev{};
    DWORD got = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &got);
    const DWORD err = ok ? 0 : GetLastError();
    CloseHandle(token);
    WA_LOG(wa::log::Level::Debug, "Elevate", "TokenElevation",
           ok ? (elev.TokenIsElevated ? "elevated" : "not-elevated") : "fail",
           ok ? "OK" : ("err=" + std::to_string(err)));
    return ok && elev.TokenIsElevated != 0;
}

bool resultIsAccessDenied(const Result& r) {
    if (r.ok) return false;
    const auto hr = static_cast<unsigned long>(r.code);
    return hr == static_cast<unsigned long>(E_ACCESSDENIED) ||
           hr == ERROR_ACCESS_DENIED;
}

Result runElevatedAndWait(const std::wstring& exePath, const std::wstring& params,
                          unsigned long* exitCodeOut) {
    if (exePath.empty()) {
        return Result::Fail(static_cast<long>(E_INVALIDARG),
                            "Elevate: empty executable path");
    }
    WA_LOG(wa::log::Level::Info, "Elevate", "ShellExecuteEx(runas)",
           "exe=" + narrowForLog(exePath) + " params=" + narrowForLog(params), "start");

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exePath.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei)) {
        const DWORD err = GetLastError();
        WA_LOG(wa::log::Level::Err, "Elevate", "ShellExecuteEx(runas)",
               narrowForLog(params), "err=" + std::to_string(err));
        if (err == ERROR_CANCELLED) {
            return Result::Fail(static_cast<long>(err),
                                "Elevate: UAC elevation cancelled");
        }
        return Result::Fail(static_cast<long>(err),
                            "Elevate: ShellExecuteEx(runas) failed");
    }

    DWORD exitCode = 1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        GetExitCodeProcess(sei.hProcess, &exitCode);
        CloseHandle(sei.hProcess);
    }
    if (exitCodeOut) *exitCodeOut = exitCode;
    WA_LOG(wa::log::Level::Info, "Elevate", "ShellExecuteEx(runas)",
           "exit=" + std::to_string(exitCode), exitCode == 0 ? "OK" : "fail");
    if (exitCode != 0) {
        return Result::Fail(static_cast<long>(exitCode),
                            "Elevate: elevated process exited with code " +
                                std::to_string(exitCode));
    }
    return Result::Ok();
}

std::wstring siblingCliPath() {
    wchar_t module[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        WA_LOG(wa::log::Level::Warn, "Elevate", "GetModuleFileNameW", "",
               "err=" + std::to_string(GetLastError()));
        return {};
    }
    std::wstring path(module, module + n);
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    path.resize(slash + 1);
    path += L"WinAudioCli.exe";
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        WA_LOG(wa::log::Level::Warn, "Elevate", "siblingCliPath",
               narrowForLog(path), "missing");
        return {};
    }
    return path;
}

}  // namespace wa
