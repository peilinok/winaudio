#pragma once
#include "PipelineGraph.h"
#include "Result.h"
#include <memory>
#include <vector>

namespace wa {

// MTA subscription to Live session create, drop-row (disconnected / Expired),
// and cell updates (volume, mute, Active/Inactive) for ACTIVE capture and
// render endpoints. A device that becomes ACTIVE after start is registered
// the same way; one that leaves ACTIVE is unregistered. Arrival and removal
// only set the sticky dirty flag so the GUI re-enumerates. They do not splice
// rows. Default-device change is ignored and does not mark the list dirty.
// Callbacks do not enumerate, touch the GUI, wait, or query session state.
// OnSessionCreated only AddRefs the new control onto a queue; the worker
// registers the per-session sink. Endpoint callbacks only wake that worker.
// Disconnect and Expired only set the dirty flag and PostMessage the main
// window. Volume, mute, and Active/Inactive store into a slot allocated at
// registration and PostMessage that slot. They do not set the dirty flag and
// do not allocate. Ducking is not subscribed. consumeDirty collapses many
// marks into one true result on the GUI thread.

// Posted to the main hwnd as a wake. The GUI applies the flag once per frame.
unsigned liveSessionDirtyMessage();

// Posted when a volume, mute, or Active/Inactive cell changes. lParam is the
// session's cell slot. Does not mark the list dirty. drainCellPatches copies
// the queued slots out.
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
