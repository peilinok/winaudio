#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wrl/client.h>
#include <string>
#include "Result.h"

namespace wa {

template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

struct ComInitGuard {
    HRESULT hr;
    ComInitGuard() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComInitGuard() { if (SUCCEEDED(hr)) CoUninitialize(); }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

inline Result HrToResult(HRESULT hr, const char* where) {
    if (SUCCEEDED(hr)) return Result::Ok();
    char buf[256];
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "%s failed: hr=0x%08lX", where,
                static_cast<unsigned long>(hr));
    return Result::Fail(static_cast<long>(hr), buf);
}

} // namespace wa
