#include "PipelineAttachSet.h"
#include <windows.h>
#include <algorithm>

namespace wa {

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
    it->second->setPumpEnabled(false);
    it->second->setCaptureEnabled(false);
    it->second->stop();
    byPid_.erase(it);
}

void PipelineAttachSet::stopAll() {
    for (auto& kv : byPid_) {
        kv.second->setPumpEnabled(false);
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
    for (auto& kv : byPid_) {
        if (!kv.second || !kv.second->attached()) continue;
        auto chunk = kv.second->drain();
        for (auto& c : chunk) {
            if (c.pid == 0) c.pid = kv.first;
            out.push_back(std::move(c));
        }
    }
    return out;
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

uint32_t PipelineAttachSet::pumpXrunsSum() const {
    uint32_t sum = 0;
    for (const auto& kv : byPid_) {
        if (kv.second && kv.second->attached()) sum += kv.second->pumpXruns();
    }
    return sum;
}

void PipelineAttachSet::setPumpEnabled(bool on) {
    for (auto& kv : byPid_) {
        if (kv.second) kv.second->setPumpEnabled(on);
    }
}

void PipelineAttachSet::setCaptureEnabled(bool on) {
    for (auto& kv : byPid_) {
        if (kv.second) kv.second->setCaptureEnabled(on);
    }
}

}  // namespace wa
