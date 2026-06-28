#pragma once
#include <string>
#include <utility>

namespace wa {

// Core never throws across its public API and never prints. Operations that
// can fail return Result; UI layers turn it into log lines / stderr.
struct Result {
    bool        ok = true;
    long        code = 0;     // HRESULT or custom error code; 0 on success
    std::string message;

    static Result Ok() { return Result{true, 0, {}}; }
    static Result Fail(long code, std::string message) {
        return Result{false, code, std::move(message)};
    }

    explicit operator bool() const { return ok; }
};

} // namespace wa
