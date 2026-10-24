#include "PumpWindow.h"
#include <algorithm>
#include <cstring>

namespace wa {
namespace {
template <size_t N>
std::string boundedText(const char (&text)[N]) { return std::string(text, strnlen(text, N)); }
}

HookedCall decodeHookedCallPod(const hook_ipc::CallPod& p) {
    HookedCall c;
    c.streamId = p.streamId;
    c.timeMs = p.timeMs;
    c.iface = boundedText(p.iface);
    c.method = boundedText(p.method);
    c.args = boundedText(p.args);
    c.hresult = p.hresult;
    c.pump = p.pump != 0;
    c.xrun = p.xrun != 0;
    if (p.hasCategory) c.category = boundedText(p.category);
    if (p.hasRaw) c.raw = p.raw != 0;
    if (p.hasMatchFormat) c.matchFormat = p.matchFormat != 0;
    if (p.hasExclusive) c.exclusive = p.exclusive != 0;
    if (p.hasFormat) c.format = boundedText(p.format);
    if (p.argBlobLen > 0 && p.argBlobLen <= sizeof(p.argBlob))
        decodeCallArgBlob(p.argBlob, p.argBlobLen, c.parameters);
    return c;
}

void PumpWindow::begin(const hook_ipc::Ring& ring) {
    boundary_ = lastWrite_ = ReadULongAcquire(reinterpret_cast<const volatile ULONG*>(&ring.pumpWriteIndex));
    std::memcpy(initial_.data(), ring.pumpSlots, sizeof(ring.pumpSlots));
    for (auto& slot : slots_) slot = {};
    current_.clear();
}

const std::vector<PumpCall>& PumpWindow::poll(const hook_ipc::Ring& ring) {
    const uint32_t write = ReadULongAcquire(reinterpret_cast<const volatile ULONG*>(&ring.pumpWriteIndex));
    // Reset/wrap has no provable continuity. Preserve monotonically new local IDs.
    if (write < lastWrite_) begin(ring);
    lastWrite_ = write;
    current_.clear();
    if (!ReadULongAcquire(reinterpret_cast<const volatile ULONG*>(&ring.pumpEnabled))) return current_;
    const uint32_t count = (std::min)(write - boundary_, hook_ipc::kPumpCap);
    for (uint32_t offset = count; offset > 0; --offset) {
        const uint32_t logical = write - offset;
        const size_t index = logical % hook_ipc::kPumpCap;
        const auto pod = ring.pumpSlots[index];
        // Prevent the compiler/CPU from folding the second shared-memory read.
        // This still cannot prove publication with the existing IPC protocol.
        MemoryBarrier();
        const auto again = ring.pumpSlots[index];
        if (std::memcmp(&pod, &again, sizeof(pod)) != 0 ||
            std::memcmp(&pod, &initial_[index], sizeof(pod)) == 0 || pod.method[0] == 0) continue;
        auto& slot = slots_[index];
        if (slot.valid && slot.logical != logical && std::memcmp(&slot.pod, &pod, sizeof(pod)) == 0)
            continue;
        if (!slot.valid || slot.logical != logical || std::memcmp(&slot.pod, &pod, sizeof(pod)) != 0) {
            slot.valid = true;
            slot.logical = logical;
            slot.sequence = ++nextSequence_;
            slot.pod = pod;
            slot.call = decodeHookedCallPod(pod);
            slot.call.pump = true;
        }
        current_.push_back({slot.sequence, slot.call});
    }
    return current_;
}

}  // namespace wa
