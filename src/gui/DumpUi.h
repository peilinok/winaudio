#pragma once
#include <string>

namespace wa::dump_ui {

std::wstring downloadsFolder();
std::wstring storedDumpFolder();          // raw HKCU value; empty if missing
std::wstring loadDumpFolder();            // existing dir, else Downloads
void         saveDumpFolder(const std::wstring& folder);
bool         pickDumpFolder(std::wstring& folder); // false = cancel
void         revealDumpFile(const std::wstring& path);

} // namespace wa::dump_ui
