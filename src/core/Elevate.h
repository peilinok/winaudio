#pragma once
#include <string>
#include "Result.h"

namespace wa {

// True when the current process token has an elevated admin group.
bool processIsElevated();

// ShellExecuteExW with verb "runas", wait for the child, return its exit code.
// UAC cancel maps to ERROR_CANCELLED. exePath must be an absolute path.
Result runElevatedAndWait(const std::wstring& exePath, const std::wstring& params,
                          unsigned long* exitCodeOut = nullptr);

// Absolute path of WinAudioCli.exe beside the current module (GUI or CLI).
// Empty when GetModuleFileName fails or the sibling is missing.
std::wstring siblingCliPath();

// True when Result carries E_ACCESSDENIED / ERROR_ACCESS_DENIED.
bool resultIsAccessDenied(const Result& r);

}  // namespace wa
