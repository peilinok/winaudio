#include "HookModuleIdentity.h"
#include "HookedCallPod.h"
#include "Log.h"
#include "ComUtil.h"
#include "AudioFormatStr.h"
#include <wincrypt.h>
#include <tlhelp32.h>
#include <array>

namespace wa {
namespace {
Result observed(BOOL ok, const char* operation) {
    const DWORD error = ok ? ERROR_SUCCESS : GetLastError();
    const HRESULT hr = HRESULT_FROM_WIN32(error);
    WA_LOG(ok ? log::Level::Debug : log::Level::Warn, "Attach", operation, "", log::hrName(hr));
    return ok ? Result::Ok() : Result::Fail(static_cast<long>(hr), std::string("Attach: ") + operation);
}

Result hashFile(const std::wstring& path, std::wstring& hex) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    Result result = observed(file != INVALID_HANDLE_VALUE, "CreateFileW(hook identity)");
    if (!result) return result;
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    result = observed(CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT),
                      "CryptAcquireContextW(hook identity)");
    if (result) result = observed(CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash),
                                   "CryptCreateHash(hook identity)");
    std::array<BYTE, 16384> bytes{};
    while (result) {
        DWORD count = 0;
        result = observed(ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr),
                          "ReadFile(hook identity)");
        if (!result || count == 0) break;
        result = observed(CryptHashData(hash, bytes.data(), count, 0), "CryptHashData(hook identity)");
    }
    std::array<BYTE, 32> digest{};
    DWORD size = static_cast<DWORD>(digest.size());
    if (result) result = observed(CryptGetHashParam(hash, HP_HASHVAL, digest.data(), &size, 0),
                                   "CryptGetHashParam(hook identity)");
    if (hash) observed(CryptDestroyHash(hash), "CryptDestroyHash(hook identity)");
    if (provider) observed(CryptReleaseContext(provider, 0), "CryptReleaseContext(hook identity)");
    observed(CloseHandle(file), "CloseHandle(hook identity)");
    if (!result) return result;
    constexpr wchar_t digits[] = L"0123456789abcdef";
    hex.clear();
    for (BYTE value : digest) {
        hex += digits[value >> 4];
        hex += digits[value & 15];
    }
    return Result::Ok();
}
}  // namespace

bool isHookModuleName(const wchar_t* name) {
    const size_t length = std::wcslen(name);
    return _wcsicmp(name, L"WinAudioHook.dll") == 0 ||
           (length > 17 && _wcsnicmp(name, L"WinAudioHook-", 13) == 0 &&
            _wcsicmp(name + length - 4, L".dll") == 0);
}

Result stageHookModule(const std::wstring& source, std::wstring& staged) {
    std::wstring hash;
    Result result = hashFile(source, hash);
    if (!result) return result;
    const auto slash = source.find_last_of(L"\\/");
    staged = source.substr(0, slash == std::wstring::npos ? 0 : slash + 1) + L"WinAudioHook-" +
             std::to_wstring(hook_ipc::kRingLayout) + L"-" + hash + L".dll";
    const BOOL copied = CopyFileW(source.c_str(), staged.c_str(), TRUE);
    const DWORD error = copied ? ERROR_SUCCESS : GetLastError();
    if (!copied && error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS)
        return observed(FALSE, "CopyFileW(content-addressed hook)");
    WA_LOG(log::Level::Debug, "Attach", "CopyFileW(content-addressed hook)", narrowAscii(staged),
           copied ? "S_OK" : "already exists; verifying content");
    std::wstring stagedHash;
    result = hashFile(staged, stagedHash);
    if (!result) return result;
    if (hash != stagedHash)
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)),
                            "Attach: staged hook content mismatch; restart the target app and retry");
    return Result::Ok();
}

Result inspectHookModules(uint32_t pid, const std::wstring& expected, bool& alreadyLoaded) {
    alreadyLoaded = false;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    Result result = observed(snapshot != INVALID_HANDLE_VALUE, "CreateToolhelp32Snapshot(hook identity)");
    if (!result) return result;
    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    DWORD enumerationError = ERROR_SUCCESS;
    const auto enumerated = [&](BOOL have, const char* operation) {
        enumerationError = have ? ERROR_SUCCESS : GetLastError();
        WA_LOG(have || enumerationError == ERROR_NO_MORE_FILES ? log::Level::Debug : log::Level::Warn,
               "Attach", operation, "pid=" + std::to_string(pid) + " snapshot=" +
                   std::to_string(reinterpret_cast<uintptr_t>(snapshot)),
               std::string(have ? "TRUE " : "FALSE ") + log::hrName(HRESULT_FROM_WIN32(enumerationError)));
        return have;
    };
    for (BOOL have = enumerated(Module32FirstW(snapshot, &entry), "Module32FirstW(hook identity)");
         have; have = enumerated(Module32NextW(snapshot, &entry), "Module32NextW(hook identity)")) {
        if (!isHookModuleName(entry.szModule)) continue;
        WA_LOG(log::Level::Info, "Attach", "loaded hook identity", std::to_string(pid), narrowAscii(entry.szExePath));
        if (_wcsicmp(entry.szExePath, expected.c_str()) != 0) {
            result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH)),
                "Attach: incompatible hook already loaded; restart the target app and retry");
            break;
        }
        alreadyLoaded = true;
    }
    if (result && enumerationError != ERROR_NO_MORE_FILES)
        result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(enumerationError)),
                              "Attach: cannot verify loaded hook identity");
    observed(CloseHandle(snapshot), "CloseHandle(hook identity snapshot)");
    return result;
}
}  // namespace wa
