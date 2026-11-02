#include "PipelineAttachSet.h"
#include "CallLogStore.h"
#include <windows.h>
#include <algorithm>

namespace wa {
Result PipelineAttachSet::publishMethods(ProcessInstance expected, uint32_t methods) {
    const auto found = byPid_.find(expected.pid);
    return found == byPid_.end()
        ? Result::Fail(static_cast<long>(E_INVALIDARG), "Monitor selection: no active target")
        : found->second->publishMethods(expected, methods);
}

Result PipelineAttachSet::adopt(std::unique_ptr<OnDemandAttach> attach) {
    if (!attach || !attach->attached() || attached(attach->pid()))
        return Result::Fail(static_cast<long>(E_INVALIDARG), "Attach: invalid handoff");
    const auto pid = attach->pid();
    byPid_[pid] = std::move(attach);
    return Result::Ok();
}

std::vector<uint32_t> PipelineAttachSet::exitedPids() const {
    std::vector<uint32_t> out;
    for (const auto& item : byPid_)
        if (item.second->targetExited()) out.push_back(item.first);
    return out;
}

std::vector<std::pair<uint32_t, std::vector<PumpCall>>> PipelineAttachSet::pumpWindows() const {
    std::vector<std::pair<uint32_t, std::vector<PumpCall>>> out;
    for (const auto& item : byPid_)
        out.emplace_back(item.first, item.second->pumpWindow());
    return out;
}

Result PipelineAttachSet::start(uint32_t pid, const std::string& deviceIdUtf8, PipelineFlow flow) {
    if (pid == 0) return Result::Fail(static_cast<long>(E_INVALIDARG), attachBlockText(AttachBlock::PidZero));
    if (attached(pid)) return Result::Ok();  // already hooked; keep existing

    auto attach = std::make_unique<OnDemandAttach>();
    Result r = attach->start(pid, deviceIdUtf8, flow);
    if (!r) return r;
    byPid_[pid] = std::move(attach);
    return Result::Ok();
}

void PipelineAttachSet::stop(uint32_t pid) {
    auto it = byPid_.find(pid);
    if (it == byPid_.end()) return;
    it->second->setCaptureEnabled(false);
    it->second->stop();
    byPid_.erase(it);
}

ProcessInstance PipelineAttachSet::instance(uint32_t pid) const {
    const auto found = byPid_.find(pid);
    return found == byPid_.end() ? ProcessInstance{} : found->second->instance();
}

std::string PipelineAttachSet::processName(uint32_t pid) const {
    const auto found = byPid_.find(pid);
    return found == byPid_.end() ? std::string{} : found->second->processName();
}

std::vector<HookedCall> PipelineAttachSet::finish(uint32_t pid) {
    const auto found = byPid_.find(pid);
    if (found == byPid_.end()) return {};
    // One bounded final drain. Outstanding remote calls are not awaited.
    found->second->setCaptureEnabled(false);
    auto calls = found->second->drain(true);
    for (auto& call : calls) call.pid = pid;
    return calls;
}

HookTransportStatus PipelineAttachSet::transportStatus(uint32_t pid) const {
    const auto found = byPid_.find(pid);
    return found == byPid_.end() ? HookTransportStatus{} : found->second->transportStatus();
}

std::vector<PumpCall> PipelineAttachSet::receivePump(uint32_t pid) {
    const auto found = byPid_.find(pid);
    return found == byPid_.end() ? std::vector<PumpCall>{} : found->second->receivePump();
}

void PipelineAttachSet::stopAll() {
    for (auto& kv : byPid_) {
        kv.second->setCaptureEnabled(false);
        kv.second->stop();
    }
    byPid_.clear();
}

bool PipelineAttachSet::attached(uint32_t pid) const {
    const auto it = byPid_.find(pid);
    return it != byPid_.end() && it->second && it->second->attached();
}

bool PipelineAttachSet::anyAttached() const {
    for (const auto& kv : byPid_) {
        if (kv.second && kv.second->attached()) return true;
    }
    return false;
}

std::vector<uint32_t> PipelineAttachSet::attachedPids() const {
    std::vector<uint32_t> out;
    out.reserve(byPid_.size());
    for (const auto& kv : byPid_) {
        if (kv.second && kv.second->attached()) out.push_back(kv.first);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<HookedCall> PipelineAttachSet::drainAll() {
    std::vector<HookedCall> out;
    for (const auto pid : attachedPids()) {
        auto chunk = byPid_.at(pid)->drain();
        for (auto& c : chunk) {
            c.pid = pid;
            out.push_back(std::move(c));
        }
    }
    return out;
}

void PipelineAttachSet::updatePump(CallLogStore& store) const {
    for (const auto pid : attachedPids())
        store.updatePump(store.activeAttach(pid), byPid_.at(pid)->pumpWindow());
}

std::vector<HookedCall> PipelineAttachSet::pumpRingAll() const {
    std::vector<HookedCall> out;
    for (const auto& kv : byPid_) {
        if (!kv.second || !kv.second->attached()) continue;
        auto chunk = kv.second->pumpRing();
        for (auto& c : chunk) {
            if (c.pid == 0) c.pid = kv.first;
            out.push_back(std::move(c));
        }
    }
    return out;
}

uint64_t PipelineAttachSet::droppedControlCalls() const {
    uint64_t total = 0;
    for (const auto& kv : byPid_)
        if (kv.second && kv.second->attached()) total += kv.second->droppedControlCalls();
    return total;
}

uint32_t PipelineAttachSet::pumpXrunsSum() const {
    uint32_t sum = 0;
    for (const auto& kv : byPid_) {
        if (kv.second && kv.second->attached()) sum += kv.second->pumpXruns();
    }
    return sum;
}



void PipelineAttachSet::setCaptureEnabled(bool on) {
    for (auto& kv : byPid_) {
        if (kv.second) kv.second->setCaptureEnabled(on);
    }
}

}  // namespace wa
