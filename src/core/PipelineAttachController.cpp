#include "PipelineAttachController.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace wa {
struct PipelineAttachController::Impl {
    struct Job {
        uint32_t pid = 0;
        std::string deviceId;
        PipelineFlow flow = PipelineFlow::Capture;
        ProcessInstance instance;
        std::atomic_bool cancelled{false};
    };
    struct Prepared {
        std::unique_ptr<OnDemandAttach> attach;
        Result result;
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool closing = false, installing = false, pump = false;
    std::shared_ptr<Job> job;
    std::optional<Prepared> prepared;  // One owning handoff, never a borrowed view.
    std::deque<uint32_t> stops;       // PID zero means detach all.
    std::deque<Batch> batches;
    Snapshot state;
    std::thread installer, receiver;

    Impl() : installer([this] { install(); }), receiver([this] { receive(); }) {}
    ~Impl() { shutdown(); }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
            if (job) job->cancelled.store(true);
        }
        changed.notify_all();
        if (installer.joinable()) installer.join();
        if (receiver.joinable()) receiver.join();
        std::lock_guard<std::mutex> lock(mutex);
        prepared.reset();
        job.reset();
        state.attached.clear();
        state.pendingPid = 0;
    }
    void install() {
        for (;;) {
            std::shared_ptr<Job> request;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return closing || (job && !installing); });
                if (closing) return;
                installing = true;
                request = job;
            }
            auto attach = std::make_unique<OnDemandAttach>();
            const Result result = attach->start(request->pid, request->deviceId,
                                                request->flow, &request->cancelled, request->instance);
            {
                std::lock_guard<std::mutex> lock(mutex);
                prepared.emplace(Prepared{std::move(attach), result});
            }
            // The sender never accesses the object after the owning move.
            changed.notify_all();
        }
    }
    void receive() {
        PipelineAttachSet active;
        std::unordered_map<uint32_t, uint64_t> tokens;
        uint64_t nextToken = 0;
        const auto now = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        };
        const auto attribute = [&](HookedCall& call) {
            call.attachToken = tokens.at(call.pid);
            call.processCreated = active.instance(call.pid).created;
        };
        bool appliedPump = false;
        for (;;) {
            std::deque<uint32_t> commands;
            std::optional<Prepared> handoff;
            std::shared_ptr<Job> request;
            bool outputReady = false, desiredPump = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(10));
                if (closing) break;
                outputReady = batches.size() < kQueueCapacity;
                desiredPump = pump;
                // Lifecycle results are lossless. Defer commands when the
                // bounded result queue is full, while still draining sources.
                if (outputReady) {
                    commands.swap(stops);
                    if (prepared) {
                        handoff = std::move(prepared);
                        prepared.reset();
                        request = job;
                    }
                }
            }
            Batch batch;
            batch.calls = active.drainAll();
            for (auto& call : batch.calls) attribute(call);
            if (appliedPump != desiredPump) {
                active.setPumpEnabled(desiredPump);
                appliedPump = desiredPump;
            }
            if (handoff) {
                Result result = handoff->result;
                {
                    // Commit and cancellation share one linearization point.
                    // No target operation here waits for remote installation.
                    std::lock_guard<std::mutex> lock(mutex);
                    if (closing || request->cancelled.load())
                        result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)),
                                              "Attach: cancelled; pending injection cleanup is managed");
                    if (result && handoff->attach->targetExited())
                        result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED)),
                                              "Attach: target exited before commit");
                    if (result) {
                        handoff->attach->setPumpEnabled(desiredPump);
                        result = active.adopt(std::move(handoff->attach));
                    }
                }
                Event event{request->pid, result ? EventKind::Attached : EventKind::Failed, result};
                event.instance = request->instance;
                event.snapshotMs = now();
                if (result) {
                    event.token = tokens[request->pid] = ++nextToken;
                    event.name = active.processName(request->pid);
                }
                batch.events.push_back(std::move(event));
                handoff.reset();
            }
            if (outputReady) {
                const auto exited = active.exitedPids();
                commands.insert(commands.end(), exited.begin(), exited.end());
                for (const auto pid : commands) {
                    const auto targets = pid == 0 ? active.attachedPids() : std::vector<uint32_t>{pid};
                    for (const auto target : targets) {
                        if (!active.attached(target)) continue;
                        auto tail = active.finish(target);
                        for (auto& call : tail) {
                            attribute(call);
                            batch.calls.push_back(std::move(call));
                        }
                        Event event{target, EventKind::Detached, Result::Ok()};
                        event.instance = active.instance(target);
                        event.token = tokens.at(target);
                        event.name = active.processName(target);
                        event.snapshotMs = now();
                        event.exited = std::find(exited.begin(), exited.end(), target) != exited.end();
                        active.stop(target);
                        tokens.erase(target);
                        batch.events.push_back(std::move(event));
                    }
                }
            }
            for (auto& item : active.pumpWindows())
                batch.pump.emplace_back(tokens.at(item.first), std::move(item.second));
            const auto pids = active.attachedPids();
            const auto dropped = active.droppedControlCalls();
            const auto xruns = active.pumpXrunsSum();
            const auto cleanup = OnDemandAttach::reapPending();
            {
                std::lock_guard<std::mutex> lock(mutex);
                state.attached = pids;
                state.sourceDroppedCalls = dropped;
                state.pumpXruns = xruns;
                state.cleanupPending = cleanup;
                if (request) {
                    job.reset();
                    installing = false;
                    state.pendingPid = 0;
                }
                if (!batch.events.empty() || !batch.calls.empty() || !batch.pump.empty()) {
                    if (batches.size() < kQueueCapacity) batches.push_back(std::move(batch));
                    else {
                        state.droppedCalls += batch.calls.size();
                        ++state.droppedBatches;
                    }
                }
            }
            changed.notify_all();
        }
        active.stopAll();
    }
};

PipelineAttachController::PipelineAttachController() : impl_(std::make_unique<Impl>()) {}
PipelineAttachController::~PipelineAttachController() = default;

Result PipelineAttachController::start(uint32_t pid, const std::string& deviceId, PipelineFlow flow,
                                       ProcessInstance expected) {
    if (pid && !expected) {
        const auto identity = queryProcessInstance(pid, expected);
        if (!identity) return identity;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (pid == 0)
        return Result::Fail(static_cast<long>(E_INVALIDARG), attachBlockText(AttachBlock::PidZero));
    if (impl_->job || impl_->state.attached.size() >= kTargetCapacity || impl_->closing ||
        std::find(impl_->state.attached.begin(), impl_->state.attached.end(), pid) != impl_->state.attached.end()) {
        ++impl_->state.rejectedCommands;
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)), "Attach: command busy or target limit reached");
    }
    impl_->job = std::make_shared<Impl::Job>();
    impl_->job->pid = pid;
    impl_->job->deviceId = deviceId;
    impl_->job->flow = flow;
    impl_->job->instance = expected;
    impl_->state.pendingPid = pid;
    impl_->changed.notify_all();
    return Result::Ok();
}

bool PipelineAttachController::stop(uint32_t pid) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->closing) return false;
    if (impl_->job && (pid == 0 || impl_->job->pid == pid)) impl_->job->cancelled.store(true);
    if (impl_->stops.size() == kQueueCapacity) {
        ++impl_->state.rejectedCommands;
        return false;
    }
    impl_->stops.push_back(pid);
    impl_->changed.notify_all();
    return true;
}
bool PipelineAttachController::stopAll() { return stop(0); }
void PipelineAttachController::shutdown() { impl_->shutdown(); }
void PipelineAttachController::setPumpEnabled(bool on) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->pump = on;
    impl_->changed.notify_all();
}
std::vector<PipelineAttachController::Batch> PipelineAttachController::poll() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<Batch> out;
    while (!impl_->batches.empty()) {
        out.push_back(std::move(impl_->batches.front()));
        impl_->batches.pop_front();
    }
    impl_->changed.notify_all();
    return out;
}
PipelineAttachController::Snapshot PipelineAttachController::snapshot() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->state;
}
std::vector<uint32_t> PipelineAttachController::attachedPids() const { return snapshot().attached; }
bool PipelineAttachController::attached(uint32_t pid) const {
    const auto pids = attachedPids();
    return std::find(pids.begin(), pids.end(), pid) != pids.end();
}
bool PipelineAttachController::anyAttached() const { return !attachedPids().empty(); }
uint64_t PipelineAttachController::droppedControlCalls() const {
    const auto state = snapshot();
    return state.droppedCalls + state.sourceDroppedCalls;
}
uint32_t PipelineAttachController::pumpXrunsSum() const { return snapshot().pumpXruns; }
}
