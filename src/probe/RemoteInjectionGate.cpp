#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cwchar>

// Test fixture only: deliberately hold the loader lock before any remote
// LoadLibrary thread can read its parameter. No COM or audio work runs here.
BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    wchar_t base[160] = {};
    if (!GetEnvironmentVariableW(L"WA_REMOTE_GATE", base, 140)) return FALSE;
    wchar_t gateName[180] = {}, enteredName[180] = {};
    swprintf_s(gateName, L"%s-gate", base);
    swprintf_s(enteredName, L"%s-entered", base);
    HANDLE gate = OpenEventW(SYNCHRONIZE, FALSE, gateName);
    HANDLE entered = OpenEventW(EVENT_MODIFY_STATE, FALSE, enteredName);
    if (!gate || !entered) return FALSE;
    SetEvent(entered);
    const DWORD waited = WaitForSingleObject(gate, 120000);
    CloseHandle(gate);
    CloseHandle(entered);
    return waited == WAIT_OBJECT_0;
}
