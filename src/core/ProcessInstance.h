#pragma once
#include "Result.h"
#include <cstdint>
#include <string>

namespace wa {

// Creation time is the unmodified Windows FILETIME, not a display timestamp.
struct ProcessInstance {
    uint32_t pid = 0;
    uint64_t created = 0;
    explicit operator bool() const { return pid != 0 && created != 0; }
};
inline bool operator==(ProcessInstance a, ProcessInstance b) {
    return a.pid == b.pid && a.created == b.created;
}
inline bool operator!=(ProcessInstance a, ProcessInstance b) { return !(a == b); }

Result queryProcessInstance(uint32_t pid, ProcessInstance& instance, std::string* name = nullptr);

}  // namespace wa
