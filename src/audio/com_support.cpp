#include "com_support.h"

#include <combaseapi.h>
#include <iomanip>
#include <sstream>

namespace winaudio {

ScopedCoInitialize::ScopedCoInitialize(DWORD coinit) {
  hr_ = CoInitializeEx(nullptr, coinit);
  initialized_ = SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE;
}

ScopedCoInitialize::~ScopedCoInitialize() {
  if (SUCCEEDED(hr_)) {
    CoUninitialize();
  }
}

bool ScopedCoInitialize::ok() const {
  return initialized_;
}

HRESULT ScopedCoInitialize::hr() const {
  return hr_;
}

std::wstring HResultToString(HRESULT hr) {
  std::wstringstream stream;
  stream << L"0x" << std::hex << std::uppercase << hr;
  return stream.str();
}

}  // namespace winaudio
