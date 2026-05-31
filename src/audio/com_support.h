#pragma once

#include <windows.h>

#include <string>

namespace winaudio {

class ScopedCoInitialize {
 public:
  explicit ScopedCoInitialize(DWORD coinit = COINIT_MULTITHREADED);
  ~ScopedCoInitialize();

  [[nodiscard]] bool ok() const;
  [[nodiscard]] HRESULT hr() const;

 private:
  HRESULT hr_ = E_FAIL;
  bool initialized_ = false;
};

std::wstring HResultToString(HRESULT hr);

}  // namespace winaudio
