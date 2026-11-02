#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// No hooks or worker threads: represents an already resident older identity.
BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
