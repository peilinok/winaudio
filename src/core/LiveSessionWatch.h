#pragma once
#include "Result.h"
#include <memory>

namespace wa {

// MTA subscription to Live session create and drop-row (disconnected / Expired)
// for the endpoints that exist when the watch starts. Callbacks do not
// enumerate, touch the GUI, wait, or query session state. OnSessionCreated only
// AddRefs the new control onto a queue; the worker registers the drop-row sink.
// Disconnect and Expired only set a sticky dirty flag and PostMessage the main
// window. Ducking and device arrival are not subscribed. consumeDirty collapses
// many marks into one true result on the GUI thread.
class LiveSessionWatch {
public:
    LiveSessionWatch();
    ~LiveSessionWatch();
    LiveSessionWatch(const LiveSessionWatch&) = delete;
    LiveSessionWatch& operator=(const LiveSessionWatch&) = delete;

    // Register on an MTA worker. hwnd receives the dirty wake. Idempotent.
    Result start(void* hwnd);
    // Unregister off the callback thread, then join the worker.
    void stop();
    bool running() const;

    bool consumeDirty() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wa
