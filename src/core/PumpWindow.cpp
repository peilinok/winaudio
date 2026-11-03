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
    c.observationGeneration = p.observationGeneration;
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
    begin(ring, static_cast<uint32_t>(hook_ipc::monitorState(ring) >> 32));
}

void PumpWindow::begin(const hook_ipc::Ring& ring, uint32_t generation) {
    reader_.begin(ring, true, generation);
    current_.clear();
    received_.clear();
}

const std::vector<PumpCall>& PumpWindow::poll(hook_ipc::Ring& ring) {
    // A second poll before a handoff must not create an unbounded inbox.
    received_.clear();
    batch_.clear();
    reader_.poll(ring, [&](const hook_ipc::CallPod& pod, uint64_t ticket) {
        auto call = decodeHookedCallPod(pod);
        call.pump = true;
        batch_.emplace_back(ticket, std::move(call));
    });
    // A slot scan can cross the physical tail. Order this bounded completed
    // batch by tickets before assigning host receipt identities. Calls that
    // complete in a later poll remain later receipts; never await a producer.
    std::sort(batch_.begin(), batch_.end(), [](const auto& a, const auto& b) {
        return a.first != b.first && b.first - a.first < (uint64_t{1} << 63);
    });
    for (auto& item : batch_) {
        current_.push_back({++nextSequence_, std::move(item.second)});
        received_.push_back(current_.back());
        if (current_.size() > kDefaultPumpRingCap) current_.erase(current_.begin());
    }
    return current_;
}

std::vector<PumpCall> PumpWindow::takeReceived() {
    return std::move(received_);
}

}  // namespace wa
