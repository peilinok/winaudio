#pragma once
#include "Result.h"
#include <cstdint>
#include <string>

namespace wa {
// Content-addressed copies cannot silently reuse a resident DLL from an older
// build with the same protocol layout. No module is ever forcibly unloaded.
Result stageHookModule(const std::wstring& source, std::wstring& staged);
Result inspectHookModules(uint32_t pid, const std::wstring& expected, bool& alreadyLoaded);
bool isHookModuleName(const wchar_t* name);
}  // namespace wa
