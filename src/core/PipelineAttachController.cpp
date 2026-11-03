#include "PipelineAttachController.h"
#include "HookedCallPod.h"
#include "Log.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <map>
#include <filesystem>

namespace wa {
struct PipelineAttachController::Impl {
    struct HookIdentity { std::wstring path; Result result; bool frozen = false; };
    struct Job {
        uint32_t pid = 0;
        std::string deviceId;
        PipelineFlow flow = PipelineFlow::Capture;
        ProcessInstance instance;
        uint32_t methods = kDefaultMonitorMethods;
        std::atomic_bool cancelled{false};
        size_t batchIndex = kTargetCapacity;
        std::shared_ptr<HookIdentity> hook;
    };
    struct Prepared {
        std::unique_ptr<OnDemandAttach> attach;
        Result result;
        ItemState outcome = ItemState::Failed;
    };
    struct StartupJob {
        StartupRequest request;
        uint32_t methods = kDefaultMonitorMethods;
        uint32_t pid = 0;
        std::atomic_bool cancelled{false};
    };
    struct StartupPrepared {
        std::unique_ptr<OnDemandAttach> attach;
        StartupOutcome outcome;
        StartupObservation observation;
    };
    mutable std::mutex mutex;
    std::condition_variable changed;
    bool closing = false, installing = false;
    std::shared_ptr<Job> job;
    std::deque<std::shared_ptr<Job>> queued;
    BatchStatus batchStatus;
    StartupStatus startupStatus;
    std::shared_ptr<StartupJob> startupJob;
    std::optional<StartupPrepared> startupPrepared;
    std::unordered_map<uint32_t, ProcessInstance> activeInstances;
    std::optional<Prepared> prepared;  // One owning handoff, never a borrowed view.
    std::deque<uint32_t> stops;       // PID zero means detach all.
    struct Configuration { ProcessInstance instance; uint64_t token; uint32_t methods; bool clear = false; };
    std::deque<Configuration> configurations;
    std::map<std::pair<uint32_t, uint64_t>, uint32_t> lastMethods;
    std::deque<Batch> batches;
    Snapshot state;
    std::thread installer, receiver, starter;

    Impl() : installer([this] { install(); }), receiver([this] { receive(); }) {}
    ~Impl() { shutdown(); }
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = true;
            if (job) job->cancelled.store(true);
            if (startupJob) startupJob->cancelled.store(true);
            queued.clear();
        }
        changed.notify_all();
        if (installer.joinable()) installer.join();
        if (starter.joinable()) starter.join();
        if (receiver.joinable()) receiver.join();
        std::lock_guard<std::mutex> lock(mutex);
        prepared.reset();
        startupPrepared.reset();
        startupJob.reset();
        job.reset();
        state.attached.clear();
        state.pendingPid = 0;
    }
    void launch(const std::shared_ptr<StartupJob>& request) {
        auto attach = std::make_unique<OnDemandAttach>();
        std::wstring hook;
        StartupObservation observation;
        StartupOutcome outcome;
        outcome.result = OnDemandAttach::freezeHook(hook);
        if (outcome.result) {
            outcome = launchAtEntry(request->request, [&](uint32_t pid, const std::atomic_bool& cancelled) {
                ProcessInstance expected;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    expected = {startupStatus.outcome.pid, startupStatus.outcome.creationTime};
                }
                auto result = attach->start(pid, {}, PipelineFlow::Render, &cancelled, expected, request->methods, hook);
                if (!result) return result;
                const auto capability = attach->coverage();
                const auto transport = attach->transportStatus();
                if (!capability.acceptedAttempts || capability.restartRequired ||
                    !transport.statistics.known || transport.observationGeneration != attach->observationGeneration())
                    return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)),
                        "Startup: hook capability/configuration/receiver handshake failed");
                observation.hookPath = std::filesystem::path(hook).u8string();
                observation.protocolLayout = hook_ipc::kRingLayout;
                observation.acceptedAttempts = capability.acceptedAttempts;
                observation.rejectedAttempts = capability.rejectedAttempts;
                observation.capabilityError = capability.lastError;
                observation.receiverReady = true;
                LARGE_INTEGER readyCounter{};
                const BOOL measured = QueryPerformanceCounter(&readyCounter);
                const auto counterError = measured ? ERROR_SUCCESS : GetLastError();
                WA_LOG(wa::log::Level::Debug, "Startup", "QueryPerformanceCounter(receiver ready)",
                    "pid=" + std::to_string(pid), wa::log::hrName(HRESULT_FROM_WIN32(counterError)));
                if (!measured) return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(counterError)), "Startup: readiness counter unavailable");
                observation.receiverReadyCounter = readyCounter.QuadPart;
                return Result::Ok();
            }, &request->cancelled, [&](StartupStage stage, const StartupOutcome& current) {
                std::lock_guard<std::mutex> lock(mutex);
                request->pid = current.pid;
                startupStatus.stage = stage;
                startupStatus.outcome = current;
                if (stage >= StartupStage::ReceiverReady) startupStatus.observation = observation;
            }, &mutex);
        }
        observation.mainThreadId = outcome.mainThreadId;
        observation.imageBase = outcome.imageBase;
        observation.entryAddress = outcome.entryAddress;
        observation.entryRva = outcome.entryRva;
        {
            std::lock_guard<std::mutex> lock(mutex);
            startupPrepared.emplace(StartupPrepared{std::move(attach), outcome, std::move(observation)});
        }
        changed.notify_all();
    }
    void install() {
        for (;;) {
            std::shared_ptr<Job> request;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait(lock, [&] { return closing || (!installing && (job || !queued.empty())); });
                if (closing) return;
                if (!job) {
                    job = queued.front();
                    queued.pop_front();
                    state.pendingPid = job->pid;
                }
                installing = true;
                request = job;
                if (request->hook) batchStatus.items[request->batchIndex].state = ItemState::Preparing;
            }
            auto attach = std::make_unique<OnDemandAttach>();
            Result result = Result::Ok();
            ItemState outcome = ItemState::Failed;
            if (request->hook && !request->hook->frozen) {
                request->hook->result = OnDemandAttach::freezeHook(request->hook->path);
                request->hook->frozen = true;
                std::lock_guard<std::mutex> lock(mutex);
                batchStatus.hookPath = request->hook->path;
            }
            if (request->hook) result = request->hook->result;
            ProcessInstance current;
            if (result) {
                result = queryProcessInstance(request->pid, current);
                if (!result) {
                    outcome = result.code == static_cast<long>(E_ACCESSDENIED) ? ItemState::Rejected : ItemState::TargetChanged;
                } else if (current != request->instance) {
                    outcome = ItemState::TargetChanged;
                    result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED)),
                                          "Attach: queued target exited or identity changed");
                }
            }
            if (result) {
                std::lock_guard<std::mutex> lock(mutex);
                if (request->cancelled.load()) {
                    outcome = ItemState::Cancelled;
                    result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)), "Attach: cancelled");
                } else if (activeInstances.count(request->pid) && activeInstances.at(request->pid) == current) {
                    outcome = ItemState::Skipped;
                } else if (activeInstances.size() + (startupJob ? 1u : 0u) >= kTargetCapacity) {
                    outcome = ItemState::Rejected;
                    result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)), "Attach: target limit reached");
                }
            }
            if (result && outcome != ItemState::Skipped) {
                result = attach->start(request->pid, request->deviceId, request->flow, &request->cancelled,
                    request->instance, request->methods, request->hook ? request->hook->path : std::wstring{},
                    [this, request](OnDemandAttach::Stage stage) {
                        if (!request->hook) return;
                        std::lock_guard<std::mutex> lock(mutex);
                        batchStatus.items[request->batchIndex].state = stage == OnDemandAttach::Stage::Injecting
                            ? ItemState::Injecting : ItemState::Installing;
                    });
                if (!result) {
                    const auto code = result.code;
                    if (code == static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED))) outcome = ItemState::Cancelled;
                    else if (code == static_cast<long>(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED))) outcome = ItemState::TargetChanged;
                    else if (code == static_cast<long>(E_ACCESSDENIED) || code == static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)) ||
                             code == static_cast<long>(HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH))) outcome = ItemState::Rejected;
                    else if (code == static_cast<long>(HRESULT_FROM_WIN32(ERROR_TIMEOUT)) ||
                             result.message.find("timed out; cleanup pending") != std::string::npos) outcome = ItemState::CleanupPending;
                }
            }
            {
                std::lock_guard<std::mutex> lock(mutex);
                prepared.emplace(Prepared{std::move(attach), result, outcome});
            }
            // The sender never accesses the object after the owning move.
            changed.notify_all();
        }
    }
    void receive() {
        PipelineAttachSet active;
        std::unordered_map<uint32_t, uint64_t> tokens;
        uint64_t nextToken = 0;
        uint64_t nextSnapshotSequence = 0;
        struct QueueLoss { uint64_t control = 0, pump = 0; };
        std::unordered_map<uint64_t, QueueLoss> queueLoss;
        const auto now = [] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
        };
        const auto attribute = [&](HookedCall& call) {
            call.attachToken = tokens.at(call.pid);
            call.processCreated = active.instance(call.pid).created;
        };
        for (;;) {
            std::deque<uint32_t> commands;
            std::deque<Configuration> updates;
            std::optional<Prepared> handoff;
            std::optional<StartupPrepared> startupHandoff;
            std::shared_ptr<Job> request;
            bool outputReady = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                changed.wait_for(lock, std::chrono::milliseconds(10));
                if (closing) break;
                outputReady = batches.size() < kQueueCapacity;
                // Lifecycle results are lossless. Defer commands when the
                // bounded result queue is full, while still draining sources.
                if (outputReady) {
                    commands.swap(stops);
                    updates.swap(configurations);
                    if (prepared) {
                        handoff = std::move(prepared);
                        prepared.reset();
                        request = job;
                    }
                    if (startupPrepared) {
                        startupHandoff = std::move(startupPrepared);
                        startupPrepared.reset();
                    }
                }
            }
            Batch batch;
            batch.calls = active.drainAll();
            for (auto& call : batch.calls) attribute(call);

            if (startupHandoff) {
                auto& launchHandoff = *startupHandoff;
                Result result = launchHandoff.outcome.result;
                Event event;
                event.pid = launchHandoff.outcome.pid;
                event.instance = {launchHandoff.outcome.pid, launchHandoff.outcome.creationTime};
                event.snapshotMs = now();
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    event.methods = startupJob->methods;
                    if (closing || startupJob->cancelled.load())
                        result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)),
                            launchHandoff.outcome.committed ? "Startup: committed target detached after cancellation"
                                                      : "Startup: cancelled before commit");
                    // The source was prepared before entry release. Adopting the
                    // owning receiver never resets its cursor or source generation.
                    if (result) result = active.adopt(std::move(launchHandoff.attach));
                    event.kind = result ? EventKind::Attached : EventKind::Failed;
                    event.result = result;
                    if (result) {
                        event.token = tokens[event.pid] = ++nextToken;
                        event.name = active.processName(event.pid);
                        event.observationGeneration = active.observationGeneration(event.pid);
                        event.startup = launchHandoff.observation;
                        activeInstances[event.pid] = event.instance;
                        lastMethods[{event.pid, event.instance.created}] = event.methods;
                    }
                    startupStatus.running = false;
                    startupStatus.outcome = launchHandoff.outcome;
                    startupStatus.outcome.result = result;
                    startupStatus.observation = launchHandoff.observation;
                    startupJob.reset();
                }
                batch.events.push_back(std::move(event));
            }

            if (handoff) {
                Result result = handoff->result;
                {
                    // Commit and cancellation share one linearization point.
                    // No target operation here waits for remote installation.
                    std::lock_guard<std::mutex> lock(mutex);
                    if (closing || request->cancelled.load()) {
                        handoff->outcome = ItemState::Cancelled;
                        result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)),
                                              "Attach: cancelled; pending injection cleanup is managed");
                    }
                    if (result && handoff->attach->targetExited()) {
                        handoff->outcome = ItemState::TargetChanged;
                        result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED)),
                                              "Attach: target exited before commit");
                    }
                    if (result && handoff->outcome != ItemState::Skipped) {
                        result = active.adopt(std::move(handoff->attach));
                        if (result) activeInstances[request->pid] = request->instance;
                    }
                    if (request->hook) {
                        auto& item = batchStatus.items[request->batchIndex];
                        item.state = result && handoff->outcome != ItemState::Skipped ? ItemState::Succeeded : handoff->outcome;
                        item.result = result;
                    }
                }
                if (handoff->outcome != ItemState::Skipped) {
                    Event event{request->pid, result ? EventKind::Attached : EventKind::Failed, result};
                    event.instance = request->instance;
                    event.snapshotMs = now();
                    event.methods = request->methods;
                    if (result) {
                        event.token = tokens[request->pid] = ++nextToken;
                        event.name = active.processName(request->pid);
                        event.observationGeneration = active.observationGeneration(request->pid);
                        std::lock_guard<std::mutex> lock(mutex);
                        lastMethods[{request->instance.pid, request->instance.created}] = request->methods;
                    }
                    batch.events.push_back(std::move(event));
                }
                handoff.reset();
            }
            if (outputReady) {
                for (const auto& update : updates) {
                    const auto found = tokens.find(update.instance.pid);
                    const Result result = found != tokens.end() && found->second == update.token
                        ? (update.clear ? active.clear(update.instance) : active.publishMethods(update.instance, update.methods))
                        : Result::Fail(static_cast<long>(E_INVALIDARG), "Monitor selection: stale segment");
                    Event event{update.instance.pid, update.clear
                        ? (result ? EventKind::Cleared : EventKind::ClearFailed)
                        : (result ? EventKind::Configured : EventKind::ConfigurationFailed), result};
                    event.instance = update.instance;
                    event.token = update.token;
                    event.methods = update.methods;
                    if (result && update.clear) {
                        queueLoss[update.token] = {};
                        event.observationGeneration = active.observationGeneration(update.instance.pid);
                        event.snapshotMs = now();
                    } else if (result) {
                        event.snapshotMs = now();
                        event.observationGeneration = active.observationGeneration(update.instance.pid);
                        std::lock_guard<std::mutex> lock(mutex);
                        lastMethods[{update.instance.pid, update.instance.created}] = update.methods;
                    }
                    batch.events.push_back(std::move(event));
                }
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
                        auto pump = active.receivePump(target);
                        if (!pump.empty()) batch.pump.emplace_back(event.token, std::move(pump));
                        auto status = active.transportStatus(target);
                        event.snapshotMs = now();
                        status.snapshotMs = event.snapshotMs;
                        status.snapshotSequence = ++nextSnapshotSequence;
                        status.control.unknown = status.pump.unknown = true;
                        status.controllerDroppedControl = queueLoss[event.token].control;
                        status.controllerDroppedPump = queueLoss[event.token].pump;
                        batch.transport.push_back({event.instance, event.token, status});
                        event.exited = std::find(exited.begin(), exited.end(), target) != exited.end();
                        active.stop(target);
                        tokens.erase(target);
                        batch.events.push_back(std::move(event));
                    }
                }
            }
            const auto pids = active.attachedPids();
            for (const auto pid : pids) {
                const auto token = tokens.at(pid);
                auto pump = active.receivePump(pid);
                if (!pump.empty()) batch.pump.emplace_back(token, std::move(pump));
                auto status = active.transportStatus(pid);
                status.snapshotMs = now();
                status.snapshotSequence = ++nextSnapshotSequence;
                status.controllerDroppedControl = queueLoss[token].control;
                status.controllerDroppedPump = queueLoss[token].pump;
                batch.transport.push_back({active.instance(pid), token, status});
            }
            const auto dropped = active.droppedControlCalls();
            const auto xruns = active.pumpXrunsSum();
            const auto cleanup = OnDemandAttach::reapPending();
            {
                std::lock_guard<std::mutex> lock(mutex);
                state.attached = pids;
                activeInstances.clear();
                for (const auto pid : pids) activeInstances.emplace(pid, active.instance(pid));
                state.sourceDroppedCalls = dropped;
                state.pumpXruns = xruns;
                state.cleanupPending = cleanup;
                if (request) {
                    job.reset();
                    installing = false;
                    state.pendingPid = 0;
                    batchStatus.running = !queued.empty();
                }
                if (!batch.events.empty() || !batch.calls.empty() || !batch.pump.empty() || !batch.transport.empty()) {
                    if (batches.size() < kQueueCapacity) {
                        for (const auto& event : batch.events)
                            if (event.kind == EventKind::Detached) queueLoss.erase(event.token);
                        batches.push_back(std::move(batch));
                    }
                    else {
                        state.droppedCalls += batch.calls.size();
                        for (const auto& call : batch.calls) ++queueLoss[call.attachToken].control;
                        for (const auto& pump : batch.pump) queueLoss[pump.first].pump += pump.second.size();
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

Result PipelineAttachController::startStartup(const StartupRequest& request, uint32_t methods) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (request.executable.empty() || !request.timeoutMs || (methods & ~kAllMonitorMethods))
        return Result::Fail(static_cast<long>(E_INVALIDARG), "Startup: invalid launch/configuration");
    if (impl_->closing || impl_->startupJob || impl_->activeInstances.size() + (impl_->job ? 1u : 0u) >= kTargetCapacity || !impl_->stops.empty())
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)), "Startup: busy or target limit reached");
    if (impl_->starter.joinable()) impl_->starter.join();
    auto job = std::make_shared<Impl::StartupJob>();
    job->request = request;
    job->methods = methods;
    impl_->startupStatus = {};
    impl_->startupStatus.running = true;
    impl_->startupJob = job;
    impl_->starter = std::thread([impl = impl_.get(), job] { impl->launch(job); });
    return Result::Ok();
}

bool PipelineAttachController::cancelStartup() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->closing || !impl_->startupJob) return false;
    impl_->startupJob->cancelled.store(true);
    impl_->changed.notify_all();
    return true;
}

PipelineAttachController::StartupStatus PipelineAttachController::startupStatus() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->startupStatus;
}

Result PipelineAttachController::startBatch(const std::vector<Target>& targets) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (targets.empty() || targets.size() > kTargetCapacity || impl_->closing || impl_->job ||
        !impl_->queued.empty() || !impl_->stops.empty())
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)), "Batch attach: empty, busy or target limit reached");
    BatchStatus status;
    auto hook = std::make_shared<Impl::HookIdentity>();
    std::deque<std::shared_ptr<Impl::Job>> queued;
    for (auto target : targets) {
        if (!target.instance || (target.methods != UINT32_MAX && (target.methods & ~kAllMonitorMethods)))
            return Result::Fail(static_cast<long>(E_INVALIDARG), "Batch attach: invalid frozen target");
        if (std::any_of(status.items.begin(), status.items.end(), [&](const Item& item) {
                return item.target.instance == target.instance;
            })) continue;
        if (target.methods == UINT32_MAX) {
            const auto found = impl_->lastMethods.find({target.instance.pid, target.instance.created});
            target.methods = found == impl_->lastMethods.end() ? kDefaultMonitorMethods : found->second;
        }
        auto request = std::make_shared<Impl::Job>();
        request->pid = target.instance.pid;
        request->instance = target.instance;
        request->deviceId = target.deviceId;
        request->flow = target.flow;
        request->methods = target.methods;
        request->hook = hook;
        request->batchIndex = status.items.size();
        status.items.push_back({std::move(target)});
        queued.push_back(std::move(request));
    }
    status.running = true;
    impl_->batchStatus = std::move(status);
    impl_->queued = std::move(queued);
    impl_->changed.notify_all();
    return Result::Ok();
}
bool PipelineAttachController::cancelBatch() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->closing) return false;
    if (impl_->job && impl_->job->hook) impl_->job->cancelled.store(true);
    for (const auto& request : impl_->queued) {
        auto& item = impl_->batchStatus.items[request->batchIndex];
        item.state = ItemState::Cancelled;
        item.result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)), "Attach: cancelled before preparation");
    }
    impl_->queued.clear();
    impl_->batchStatus.running = impl_->job && impl_->job->hook;
    impl_->changed.notify_all();
    return true;
}
PipelineAttachController::BatchStatus PipelineAttachController::batchStatus() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->batchStatus;
}

Result PipelineAttachController::start(uint32_t pid, const std::string& deviceId, PipelineFlow flow,
                                       ProcessInstance expected, uint32_t methods) {
    if (pid && !expected) {
        const auto identity = queryProcessInstance(pid, expected);
        if (!identity) return identity;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (pid == 0)
        return Result::Fail(static_cast<long>(E_INVALIDARG), attachBlockText(AttachBlock::PidZero));
    if (impl_->job || !impl_->queued.empty() || !impl_->stops.empty() || impl_->state.attached.size() >= kTargetCapacity || impl_->closing ||
        std::find(impl_->state.attached.begin(), impl_->state.attached.end(), pid) != impl_->state.attached.end()) {
        ++impl_->state.rejectedCommands;
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_BUSY)), "Attach: command busy or target limit reached");
    }
    impl_->job = std::make_shared<Impl::Job>();
    impl_->job->pid = pid;
    impl_->job->deviceId = deviceId;
    impl_->job->flow = flow;
    impl_->job->instance = expected;
    const auto found = impl_->lastMethods.find({expected.pid, expected.created});
    impl_->job->methods = methods == UINT32_MAX
        ? (found == impl_->lastMethods.end() ? kDefaultMonitorMethods : found->second) : methods;
    impl_->state.pendingPid = pid;
    impl_->changed.notify_all();
    return Result::Ok();
}

uint32_t PipelineAttachController::lastMethods(ProcessInstance instance) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto found = impl_->lastMethods.find({instance.pid, instance.created});
    return found == impl_->lastMethods.end() ? kDefaultMonitorMethods : found->second;
}
Result PipelineAttachController::configure(ProcessInstance instance, uint64_t token, uint32_t methods) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!instance || !token || (methods & ~kAllMonitorMethods) || impl_->closing ||
        impl_->configurations.size() >= kQueueCapacity)
        return Result::Fail(static_cast<long>(E_INVALIDARG), "Monitor selection: invalid or busy command");
    impl_->configurations.push_back({instance, token, methods});
    impl_->changed.notify_all();
    return Result::Ok();
}

Result PipelineAttachController::clear(ProcessInstance instance, uint64_t token) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!instance || !token || impl_->closing || impl_->configurations.size() >= kQueueCapacity)
        return Result::Fail(static_cast<long>(E_INVALIDARG), "Clear segment: invalid or busy command");
    impl_->configurations.push_back({instance, token, 0, true});
    impl_->changed.notify_all();
    return Result::Ok();
}

bool PipelineAttachController::stop(uint32_t pid) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->closing) return false;
    if (impl_->job && (pid == 0 || impl_->job->pid == pid)) impl_->job->cancelled.store(true);
    if (impl_->startupJob && (pid == 0 || impl_->startupJob->pid == pid)) impl_->startupJob->cancelled.store(true);
    for (auto it = impl_->queued.begin(); it != impl_->queued.end();) {
        const auto& request = *it;
        if (pid != 0 && request->pid != pid) { ++it; continue; }
        auto& item = impl_->batchStatus.items[request->batchIndex];
        item.state = ItemState::Cancelled;
        item.result = Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_CANCELLED)), "Attach: cancelled before preparation");
        it = impl_->queued.erase(it);
    }
    if (!impl_->job && impl_->queued.empty()) impl_->batchStatus.running = false;
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
