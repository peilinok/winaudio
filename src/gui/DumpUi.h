#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace wa::dump_ui {

std::wstring downloadsFolder();
std::wstring storedDumpFolder();          // raw HKCU value; empty if missing
std::wstring loadDumpFolder();            // existing dir, else Downloads
void         saveDumpFolder(const std::wstring& folder);
bool         pickDumpFolder(std::wstring& folder); // blocking; false = cancel
void         revealDumpFile(const std::wstring& path);

// Native folder dialog on a dedicated STA thread so the GUI frame loop
// (and capture pumps) keep running. Show() is not given an owner HWND,
// so the main window is not disabled.
class FolderPicker {
public:
    FolderPicker() = default;
    ~FolderPicker();
    FolderPicker(const FolderPicker&)            = delete;
    FolderPicker& operator=(const FolderPicker&) = delete;

    bool start(const std::wstring& initialFolder); // false if already busy
    bool busy() const { return busy_.load(std::memory_order_acquire); }
    bool take(std::wstring& folder, bool& accepted); // true if a pick finished

private:
    void worker(std::wstring initial);

    std::mutex          mtx_;
    std::thread         thread_;
    std::atomic<bool>   busy_{false};
    std::atomic<bool>   done_{false};
    bool                accepted_ = false;
    std::wstring        folder_;
};

} // namespace wa::dump_ui
