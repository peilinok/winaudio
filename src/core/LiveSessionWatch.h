#pragma once
#include "PipelineGraph.h"
#include "Result.h"
#include <memory>
#include <vector>

namespace wa {

// MTA subscription to Live session create, drop-row (disconnected / Expired),
// and cell updates (volume, mute, Active/Inactive) for the endpoints that
// exist when the watch starts. Callbacks do not enumerate, touch the GUI,
// wait, or query session state. OnSessionCreated only AddRefs the new control
// onto a queue; the worker registers the per-session sink. Disconnect and
// Expired only set a sticky dirty flag and PostMessage the main window.
// Volume, mute, and Active/Inactive post a cell patch and do not set that
// flag. Ducking and device arrival are not subscribed. consumeDirty collapses
// many marks into one true result on the GUI thread.

// Posted to the main hwnd as a wake. The GUI applies the flag once per frame.
unsigned liveSessionDirtyMessage();

// Posted when a volume, mute, or Active/Inactive cell changes. Does not mark
// the list dirty. drainCellPatches returns those patches oldest first.
unsigned liveSessionCellMessage();

class LiveSessionWatch {
public:
    LiveSessionWatch();
    ~LiveSessionWatch();
    LiveSessionWatch(const LiveSessionWatch&) = delete;
    LiveSessionWatch& operator=(const LiveSessionWatch&) = delete;

    // Register on an MTA worker. hwnd receives the dirty and cell wakes. Idempotent.
    Result start(void* hwnd);
    // Unregister off the callback thread, then join the worker.
    void stop();
    bool running() const;

    bool consumeDirty() noexcept;
    std::vector<LiveSessionCellPatch> drainCellPatches();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wa
