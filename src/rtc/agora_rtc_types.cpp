#include "rtc/agora_rtc_types.h"

namespace winaudio {

std::wstring MaskAgoraToken(const std::wstring& token) {
  if (token.empty()) {
    return L"(not set)";
  }
  constexpr size_t kVisibleTail = 4;
  if (token.size() <= kVisibleTail) {
    return std::wstring(token.size(), L'*');
  }
  return std::wstring(token.size() - kVisibleTail, L'*') +
         token.substr(token.size() - kVisibleTail);
}

}  // namespace winaudio
