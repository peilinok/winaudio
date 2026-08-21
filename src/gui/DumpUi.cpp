#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "DumpUi.h"
#include "Log.h"
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>

namespace wa::dump_ui {

namespace {
constexpr wchar_t kRegKey[]   = L"Software\\WinAudio";
constexpr wchar_t kRegValue[] = L"DumpFolder";

bool isExistingDirectory(const std::wstring& path) {
    if (path.empty()) return false;
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
} // namespace

std::wstring downloadsFolder() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p)) && p) {
        out = p;
        CoTaskMemFree(p);
    }
    if (out.empty()) {
        wchar_t profile[MAX_PATH]{};
        if (GetEnvironmentVariableW(L"USERPROFILE", profile, MAX_PATH)) {
            out = std::wstring(profile) + L"\\Downloads";
        }
    }
    return out;
}

std::wstring storedDumpFolder() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return {};
    wchar_t buf[MAX_PATH]{};
    DWORD bytes = sizeof(buf);
    DWORD type = 0;
    const LONG er = RegQueryValueExW(key, kRegValue, nullptr, &type,
                                     reinterpret_cast<LPBYTE>(buf), &bytes);
    RegCloseKey(key);
    if (er != ERROR_SUCCESS || type != REG_SZ || bytes < sizeof(wchar_t)) return {};
    const size_t chars = bytes / sizeof(wchar_t);
    if (chars > 0 && buf[chars - 1] == L'\0')
        return std::wstring(buf, chars - 1);
    return std::wstring(buf, chars);
}

std::wstring loadDumpFolder() {
    const std::wstring stored = storedDumpFolder();
    if (isExistingDirectory(stored)) return stored;
    return downloadsFolder();
}

void saveDumpFolder(const std::wstring& folder) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) {
        WA_LOG(wa::log::Level::Warn, "DumpUi", "saveDumpFolder", "", "RegCreateKeyEx failed");
        return;
    }
    const DWORD bytes = static_cast<DWORD>((folder.size() + 1) * sizeof(wchar_t));
    const LONG er = RegSetValueExW(key, kRegValue, 0, REG_SZ,
                                   reinterpret_cast<const BYTE*>(folder.c_str()), bytes);
    RegCloseKey(key);
    if (er != ERROR_SUCCESS)
        WA_LOG(wa::log::Level::Warn, "DumpUi", "saveDumpFolder", "", "RegSetValueEx failed");
}

bool pickDumpFolder(std::wstring& folder) {
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = (hrInit == S_OK || hrInit == S_FALSE);

    IFileOpenDialog* dlg = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dlg));
    bool ok = false;
    if (SUCCEEDED(hr) && dlg) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        const std::wstring start = folder.empty() ? loadDumpFolder() : folder;
        if (!start.empty()) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(start.c_str(), nullptr,
                                                      IID_PPV_ARGS(&item))) && item) {
                dlg->SetFolder(item);
                item->Release();
            }
        }
        // No owner: do not disable the GUI window (caller may be a worker thread).
        hr = dlg->Show(nullptr);
        if (SUCCEEDED(hr)) {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dlg->GetResult(&result)) && result) {
                PWSTR path = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    folder = path;
                    ok = !folder.empty();
                    CoTaskMemFree(path);
                }
                result->Release();
            }
        }
        dlg->Release();
    } else {
        WA_LOG(wa::log::Level::Err, "DumpUi", "pickDumpFolder", "", "CoCreateInstance failed");
    }
    if (uninit) CoUninitialize();
    return ok;
}

FolderPicker::~FolderPicker() {
    if (thread_.joinable()) thread_.join();
}

bool FolderPicker::start(const std::wstring& initialFolder) {
    if (busy_.load(std::memory_order_acquire)) return false;
    if (thread_.joinable()) thread_.join();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        accepted_ = false;
        folder_.clear();
    }
    done_.store(false, std::memory_order_release);
    busy_.store(true, std::memory_order_release);
    thread_ = std::thread(&FolderPicker::worker, this, initialFolder);
    return true;
}

bool FolderPicker::take(std::wstring& folder, bool& accepted) {
    if (!done_.load(std::memory_order_acquire)) return false;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lk(mtx_);
    folder = folder_;
    accepted = accepted_;
    done_.store(false, std::memory_order_release);
    return true;
}

void FolderPicker::worker(std::wstring initial) {
    wa::log::setThreadName("dump-ui");
    std::wstring folder = initial;
    const bool ok = pickDumpFolder(folder);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        accepted_ = ok;
        folder_ = ok ? folder : std::wstring{};
    }
    done_.store(true, std::memory_order_release);
    busy_.store(false, std::memory_order_release);
}

void revealDumpFile(const std::wstring& path) {
    if (path.empty()) return;
    const std::wstring args = L"/select,\"" + path + L"\"";
    const INT_PTR r = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
    if (r <= 32)
        WA_LOG(wa::log::Level::Warn, "DumpUi", "revealDumpFile", "", "ShellExecute failed");
}

} // namespace wa::dump_ui
