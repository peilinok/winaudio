#include "LiveSessionWatch.h"
#include <atomic>
#include <cstring>
#include <malloc.h>
#include <new>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "Log.h"

namespace wa {

constexpr UINT kLiveSessionDirtyMessage = WM_APP + 0x4157;
constexpr UINT kLiveSessionCellMessage = WM_APP + 0x4158;
// Callbacks borrow a node. They do not allocate. 128 covers a burst of creates
// before the worker recycles; past that the worker enumerates once.
constexpr int kCreateNodePool = 128;

unsigned liveSessionDirtyMessage() { return kLiveSessionDirtyMessage; }
unsigned liveSessionCellMessage() { return kLiveSessionCellMessage; }

namespace {

class CreateSink;
class SessionEvents;

struct PendingSession {
    SLIST_ENTRY entry;
    IAudioSessionControl* control;
    const std::string* deviceId;
    PipelineFlow flow;
};

// Heap node. The callback copies identity captured at registration; the GUI
// drains oldest-first. SLIST is LIFO, so drain reverses a popped batch.
struct CellNode {
    SLIST_ENTRY entry;
    LiveSessionCellPatch patch;
};

struct SessionIdentity {
    std::string instanceId;
    std::string deviceId;
    uint32_t processId = 0;
    PipelineFlow flow = PipelineFlow::Capture;
};

std::string utf8FromWide(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

struct WatchState {
    // SLIST_HEADER requires 16-byte alignment and stays the first members.
    SLIST_HEADER pending{};
    SLIST_HEADER freeNodes{};
    SLIST_HEADER cells{};
    std::atomic<bool> stopping{false};
    std::atomic<bool> dirty{false};
    std::atomic<bool> posted{false};
    std::atomic<bool> cellPosted{false};
    std::atomic<int> droppedCreates{0};
    HWND hwnd = nullptr;
    HANDLE wake = nullptr;

    WatchState() {
        InitializeSListHead(&pending);
        InitializeSListHead(&freeNodes);
        InitializeSListHead(&cells);
    }

    void postDirty() noexcept {
        if (stopping.load(std::memory_order_acquire)) return;
        dirty.store(true, std::memory_order_release);
        bool expected = false;
        if (posted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (hwnd) PostMessage(hwnd, kLiveSessionDirtyMessage, 0, 0);
        }
    }

    // Volume, mute, and Active/Inactive. Does not set the dirty flag.
    // One wake is enough until the GUI drains; the patches themselves queue up.
    bool postCell(LiveSessionCellPatch patch) {
        if (stopping.load(std::memory_order_acquire)) return false;
        void* mem = _aligned_malloc(sizeof(CellNode), MEMORY_ALLOCATION_ALIGNMENT);
        if (!mem) return false;
        auto* node = new (mem) CellNode();
        node->patch = std::move(patch);
        InterlockedPushEntrySList(&cells, &node->entry);
        bool expected = false;
        if (cellPosted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (hwnd) PostMessage(hwnd, kLiveSessionCellMessage, 0, 0);
        }
        return true;
    }

    // Lock-free. The caller already AddRef'd control. False means the caller
    // still owns that reference (pool empty or stopping). deviceId must outlive
    // the queued node (the per-endpoint create sink's own string).
    bool enqueue(IAudioSessionControl* control, const std::string* deviceId, PipelineFlow flow) {
        if (!control || stopping.load(std::memory_order_acquire)) return false;
        SLIST_ENTRY* entry = InterlockedPopEntrySList(&freeNodes);
        if (!entry) return false;
        auto* node = reinterpret_cast<PendingSession*>(entry);
        node->control = control;
        node->deviceId = deviceId;
        node->flow = flow;
        InterlockedPushEntrySList(&pending, &node->entry);
        if (wake) SetEvent(wake);
        return true;
    }

    void noteDroppedCreate() noexcept {
        droppedCreates.fetch_add(1, std::memory_order_relaxed);
        if (wake) SetEvent(wake);
    }
};

class SessionEvents final : public IAudioSessionEvents {
public:
    SessionEvents(WatchState* state, SessionIdentity id) : state_(state), id_(std::move(id)) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionEvents)) {
            *ppv = static_cast<IAudioSessionEvents*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP OnDisplayNameChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnIconPathChanged(LPCWSTR, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnSimpleVolumeChanged(float newVolume, BOOL mute, LPCGUID) override {
        if (!state_) return S_OK;
        LiveSessionCellPatch patch = identityPatch();
        patch.hasVolume = true;
        patch.volume = newVolume;
        patch.hasMute = true;
        patch.mute = mute != FALSE;
        const bool queued = state_->postCell(std::move(patch));
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "OnSimpleVolumeChanged",
               "vol=" + std::to_string(newVolume) + " mute=" + (mute ? "true" : "false"),
               queued ? "queued" : "dropped");
        return S_OK;
    }
    STDMETHODIMP OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnStateChanged(AudioSessionState newState) override {
        if (!state_) return S_OK;
        if (newState == AudioSessionStateExpired) {
            state_->postDirty();
            WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "OnStateChanged",
                   "Expired", "dirty");
            return S_OK;
        }
        if (newState != AudioSessionStateActive && newState != AudioSessionStateInactive)
            return S_OK;
        const char* name = newState == AudioSessionStateActive ? "Active" : "Inactive";
        LiveSessionCellPatch patch = identityPatch();
        patch.hasState = true;
        patch.state = name;
        const bool queued = state_->postCell(std::move(patch));
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "OnStateChanged",
               name, queued ? "queued" : "dropped");
        return S_OK;
    }
    STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override {
        if (state_) state_->postDirty();
        return S_OK;
    }

private:
    LiveSessionCellPatch identityPatch() const {
        LiveSessionCellPatch patch;
        patch.sessionInstanceId = id_.instanceId;
        patch.processId = id_.processId;
        patch.deviceId = id_.deviceId;
        patch.flow = id_.flow;
        return patch;
    }

    WatchState* state_ = nullptr;
    SessionIdentity id_;
    LONG refs_ = 1;
};

class CreateSink final : public IAudioSessionNotification {
public:
    CreateSink(WatchState* state, std::string deviceId, PipelineFlow flow)
        : state_(state), deviceId_(std::move(deviceId)), flow_(flow) {}

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioSessionNotification)) {
            *ppv = static_cast<IAudioSessionNotification*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
        const ULONG n = InterlockedDecrement(&refs_);
        if (n == 0) delete this;
        return n;
    }

    STDMETHODIMP OnSessionCreated(IAudioSessionControl* created) override {
        if (!created || !state_) return S_OK;
        // Keep the control alive for the worker. Do not query it here.
        created->AddRef();
        if (!state_->enqueue(created, &deviceId_, flow_)) {
            created->Release();
            state_->noteDroppedCreate();
        }
        state_->postDirty();
        return S_OK;
    }

private:
    WatchState* state_ = nullptr;
    std::string deviceId_;
    PipelineFlow flow_ = PipelineFlow::Capture;
    LONG refs_ = 1;
};

struct EndpointSub {
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<CreateSink> createSink;
    std::string deviceId;
    PipelineFlow flow = PipelineFlow::Capture;
};

struct WatchedSession {
    ComPtr<IAudioSessionControl> control;
    ComPtr<SessionEvents> events;
};

}  // namespace

struct LiveSessionWatch::Impl {
    WatchState state;
    std::thread worker;
    HANDLE started = nullptr;
    bool running = false;
    bool failed = false;
    long errorCode = E_FAIL;
    std::string error;

    std::vector<EndpointSub> endpoints;
    std::vector<WatchedSession> watched;
    std::vector<std::string> watchedIds;

    ~Impl() { shutdown(); }

    void shutdown() {
        state.stopping.store(true, std::memory_order_release);
        if (state.wake) SetEvent(state.wake);
        if (worker.joinable()) worker.join();
        if (state.wake) {
            CloseHandle(state.wake);
            state.wake = nullptr;
        }
        if (started) {
            CloseHandle(started);
            started = nullptr;
        }
        freeNodePool();
        freeCellNodes();
        running = false;
    }

    bool preallocateNodes() {
        for (int i = 0; i < kCreateNodePool; ++i) {
            void* mem = _aligned_malloc(sizeof(PendingSession), MEMORY_ALLOCATION_ALIGNMENT);
            if (!mem) return false;
            std::memset(mem, 0, sizeof(PendingSession));
            InterlockedPushEntrySList(&state.freeNodes,
                                      static_cast<SLIST_ENTRY*>(mem));
        }
        return true;
    }

    void freeNodePool() {
        auto releaseList = [](SLIST_HEADER* header) {
            for (;;) {
                SLIST_ENTRY* entry = InterlockedPopEntrySList(header);
                if (!entry) break;
                auto* node = reinterpret_cast<PendingSession*>(entry);
                if (node->control) {
                    node->control->Release();
                    node->control = nullptr;
                }
                _aligned_free(node);
            }
        };
        releaseList(&state.pending);
        releaseList(&state.freeNodes);
    }

    void freeCellNodes() {
        for (;;) {
            SLIST_ENTRY* entry = InterlockedPopEntrySList(&state.cells);
            if (!entry) break;
            auto* node = reinterpret_cast<CellNode*>(entry);
            node->~CellNode();
            _aligned_free(node);
        }
    }

    void threadMain() {
        wa::log::setThreadName("sessW");
        const HRESULT comHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool ownCom = (comHr == S_OK);
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "CoInitializeEx", "MTA",
               wa::log::hrName(comHr));
        if (comHr == RPC_E_CHANGED_MODE) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "CoInitializeEx", "MTA",
                   wa::log::hrName(comHr));
        }
        if (FAILED(comHr) && comHr != RPC_E_CHANGED_MODE) {
            errorCode = static_cast<long>(comHr);
            error = HrToResult(comHr, "LiveSessionWatch: CoInitializeEx").message;
            failed = true;
            SetEvent(started);
            return;
        }

        const Result subscribed = subscribeExisting();
        if (!subscribed) {
            errorCode = subscribed.code;
            error = subscribed.message;
            failed = true;
            releaseSubscriptions();
            if (ownCom) {
                WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "CoUninitialize", "", "ok");
                CoUninitialize();
            }
            SetEvent(started);
            return;
        }
        SetEvent(started);

        for (;;) {
            WaitForSingleObject(state.wake, INFINITE);
            if (state.stopping.load(std::memory_order_acquire)) break;
            drainNewSessions(false);
            const int dropped = state.droppedCreates.exchange(0, std::memory_order_acq_rel);
            if (dropped > 0) {
                WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "OnSessionCreated",
                       "dropped=" + std::to_string(dropped), "pool empty");
                catchUpDroppedSessions();
            }
        }
        releaseSubscriptions();
        if (ownCom) {
            WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "CoUninitialize", "", "ok");
            CoUninitialize();
        }
    }

    Result subscribeExisting() {
        ComPtr<IMMDeviceEnumerator> devices;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      __uuidof(IMMDeviceEnumerator),
                                      reinterpret_cast<void**>(devices.GetAddressOf()));
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "CoCreateInstance(MMDeviceEnumerator)",
               "", wa::log::hrName(hr));
        if (FAILED(hr))
            return HrToResult(hr, "LiveSessionWatch: CoCreateInstance(MMDeviceEnumerator)");

        int endpointCount = 0;
        int sessions = 0;
        Result r = subscribeFlow(devices.Get(), eCapture, endpointCount, sessions);
        if (!r) return r;
        r = subscribeFlow(devices.Get(), eRender, endpointCount, sessions);
        if (!r) return r;
        WA_LOG(wa::log::Level::Info, "LiveSessionWatch", "start",
               "endpoints=" + std::to_string(endpointCount) + " sessions=" + std::to_string(sessions),
               "ok");
        return Result::Ok();
    }

    Result subscribeFlow(IMMDeviceEnumerator* devices, EDataFlow flow, int& endpointCount,
                         int& sessions) {
        ComPtr<IMMDeviceCollection> coll;
        HRESULT hr = devices->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, coll.GetAddressOf());
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "EnumAudioEndpoints",
               flow == eCapture ? "capture" : "render", wa::log::hrName(hr));
        if (FAILED(hr))
            return HrToResult(hr, "LiveSessionWatch: EnumAudioEndpoints");

        UINT n = 0;
        hr = coll->GetCount(&n);
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetCount",
               "n=" + std::to_string(n), wa::log::hrName(hr));
        if (FAILED(hr)) return HrToResult(hr, "LiveSessionWatch: GetCount");

        const PipelineFlow pipeFlow =
            flow == eCapture ? PipelineFlow::Capture : PipelineFlow::Render;
        for (UINT i = 0; i < n; ++i) {
            ComPtr<IMMDevice> dev;
            hr = coll->Item(i, dev.GetAddressOf());
            WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "Item",
                   "i=" + std::to_string(i), wa::log::hrName(hr));
            if (FAILED(hr) || !dev) {
                if (FAILED(hr))
                    WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "Item",
                           "i=" + std::to_string(i), wa::log::hrName(hr));
                continue;
            }
            if (subscribeDevice(dev.Get(), sessions, pipeFlow)) ++endpointCount;
        }
        return Result::Ok();
    }

    bool subscribeDevice(IMMDevice* dev, int& sessions, PipelineFlow pipeFlow) {
        LPWSTR id = nullptr;
        HRESULT hr = dev->GetId(&id);
        const std::string idText = (SUCCEEDED(hr) && id) ? utf8FromWide(id) : std::string();
        if (id) CoTaskMemFree(id);
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetId", idText, wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetId", idText, wa::log::hrName(hr));
        }

        ComPtr<IAudioSessionManager2> manager;
        hr = dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(manager.GetAddressOf()));
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "Activate(IAudioSessionManager2)",
               idText, wa::log::hrName(hr));
        if (FAILED(hr) || !manager) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "Activate(IAudioSessionManager2)",
                   idText, wa::log::hrName(hr));
            return false;
        }

        EndpointSub ep;
        ep.deviceId = idText;
        ep.flow = pipeFlow;
        ep.manager = manager;
        ep.createSink.Attach(new CreateSink(&state, ep.deviceId, ep.flow));
        hr = ep.manager->RegisterSessionNotification(ep.createSink.Get());
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "RegisterSessionNotification",
               idText, wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "RegisterSessionNotification",
                   idText, wa::log::hrName(hr));
            return false;
        }
        endpoints.push_back(std::move(ep));
        EndpointSub& stored = endpoints.back();
        sessions += registerSessions(stored.manager.Get(), stored.deviceId, stored.flow);
        return true;
    }

    int registerSessions(IAudioSessionManager2* manager, const std::string& deviceId,
                         PipelineFlow flow) {
        if (!manager) return 0;
        ComPtr<IAudioSessionEnumerator> sessionEnum;
        HRESULT hr = manager->GetSessionEnumerator(sessionEnum.GetAddressOf());
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetSessionEnumerator",
               deviceId, wa::log::hrName(hr));
        if (FAILED(hr) || !sessionEnum) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetSessionEnumerator",
                   deviceId, wa::log::hrName(hr));
            return 0;
        }
        int count = 0;
        hr = sessionEnum->GetCount(&count);
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetSessionCount",
               "n=" + std::to_string(count), wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetSessionCount",
                   "n=" + std::to_string(count), wa::log::hrName(hr));
            return 0;
        }

        int added = 0;
        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> control;
            hr = sessionEnum->GetSession(i, control.GetAddressOf());
            WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetSession",
                   "i=" + std::to_string(i), wa::log::hrName(hr));
            if (FAILED(hr) || !control) {
                if (FAILED(hr)) {
                    WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetSession",
                           "i=" + std::to_string(i), wa::log::hrName(hr));
                }
                continue;
            }
            if (registerDropSink(control.Get(), deviceId, flow)) ++added;
        }
        return added;
    }

    void catchUpDroppedSessions() {
        for (auto& ep : endpoints) {
            if (ep.manager) registerSessions(ep.manager.Get(), ep.deviceId, ep.flow);
        }
    }

    SessionIdentity readIdentity(IAudioSessionControl* control, const std::string& deviceId,
                                 PipelineFlow flow) {
        SessionIdentity id;
        id.deviceId = deviceId;
        id.flow = flow;
        ComPtr<IAudioSessionControl2> control2;
        HRESULT hr = control->QueryInterface(__uuidof(IAudioSessionControl2),
                                             reinterpret_cast<void**>(control2.GetAddressOf()));
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "QueryInterface(IAudioSessionControl2)",
               "", wa::log::hrName(hr));
        if (FAILED(hr) || !control2) {
            if (FAILED(hr)) {
                WA_LOG(wa::log::Level::Warn, "LiveSessionWatch",
                       "QueryInterface(IAudioSessionControl2)", "", wa::log::hrName(hr));
            }
            return id;
        }

        DWORD pid = 0;
        hr = control2->GetProcessId(&pid);
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetProcessId",
               "pid=" + std::to_string(pid), wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetProcessId",
                   "pid=" + std::to_string(pid), wa::log::hrName(hr));
        } else {
            id.processId = static_cast<uint32_t>(pid);
        }

        LPWSTR instanceWide = nullptr;
        hr = control2->GetSessionInstanceIdentifier(&instanceWide);
        id.instanceId = (SUCCEEDED(hr) && instanceWide) ? utf8FromWide(instanceWide) : std::string();
        if (instanceWide) CoTaskMemFree(instanceWide);
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetSessionInstanceIdentifier",
               id.instanceId.empty() ? "empty" : id.instanceId, wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetSessionInstanceIdentifier",
                   "empty", wa::log::hrName(hr));
        }
        return id;
    }

    bool registerDropSink(IAudioSessionControl* control, const std::string& deviceId,
                          PipelineFlow flow) {
        if (!control) return false;
        const SessionIdentity id = readIdentity(control, deviceId, flow);
        if (!id.instanceId.empty()) {
            for (const auto& seen : watchedIds) {
                if (seen == id.instanceId) return true;
            }
        }
        AudioSessionState st = AudioSessionStateInactive;
        const HRESULT stateHr = control->GetState(&st);
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetState", "", wa::log::hrName(stateHr));
        if (FAILED(stateHr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetState", "", wa::log::hrName(stateHr));
        }
        if (SUCCEEDED(stateHr) && st == AudioSessionStateExpired) return false;

        ComPtr<SessionEvents> sink;
        sink.Attach(new SessionEvents(&state, id));
        const HRESULT hr = control->RegisterAudioSessionNotification(sink.Get());
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "RegisterAudioSessionNotification",
               id.instanceId, wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "RegisterAudioSessionNotification",
                   id.instanceId, wa::log::hrName(hr));
            return false;
        }
        control->AddRef();
        ComPtr<IAudioSessionControl> held;
        held.Attach(control);
        watched.push_back(WatchedSession{std::move(held), std::move(sink)});
        if (!id.instanceId.empty()) watchedIds.push_back(id.instanceId);
        return true;
    }

    void drainNewSessions(bool releaseOnly) {
        for (;;) {
            SLIST_ENTRY* entry = InterlockedPopEntrySList(&state.pending);
            if (!entry) break;
            auto* node = reinterpret_cast<PendingSession*>(entry);
            IAudioSessionControl* control = node->control;
            const std::string* deviceId = node->deviceId;
            const PipelineFlow flow = node->flow;
            node->control = nullptr;
            node->deviceId = nullptr;
            InterlockedPushEntrySList(&state.freeNodes, &node->entry);
            if (!control) continue;
            // The queue owns one ref. A new registration AddRefs into watched;
            // an id already seen, an expired row, or releaseOnly does not.
            if (!releaseOnly)
                registerDropSink(control, deviceId ? *deviceId : std::string(), flow);
            control->Release();
        }
    }

    void releaseSubscriptions() {
        for (auto& ep : endpoints) {
            if (!ep.manager || !ep.createSink) continue;
            const HRESULT hr = ep.manager->UnregisterSessionNotification(ep.createSink.Get());
            WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "UnregisterSessionNotification",
                   "", wa::log::hrName(hr));
            if (FAILED(hr)) {
                WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "UnregisterSessionNotification",
                       "", wa::log::hrName(hr));
            }
        }
        // OnSessionCreated has returned by the time Unregister returns.
        drainNewSessions(true);
        for (auto& w : watched) {
            if (w.control && w.events) {
                const HRESULT hr = w.control->UnregisterAudioSessionNotification(w.events.Get());
                WA_LOG(wa::log::Level::Debug, "LiveSessionWatch",
                       "UnregisterAudioSessionNotification", "", wa::log::hrName(hr));
                if (FAILED(hr)) {
                    WA_LOG(wa::log::Level::Warn, "LiveSessionWatch",
                           "UnregisterAudioSessionNotification", "", wa::log::hrName(hr));
                }
            }
        }
        watched.clear();
        watchedIds.clear();
        endpoints.clear();
        WA_LOG(wa::log::Level::Info, "LiveSessionWatch", "stop", "", "ok");
    }
};

LiveSessionWatch::LiveSessionWatch() : impl_(std::make_unique<Impl>()) {}

LiveSessionWatch::~LiveSessionWatch() = default;

Result LiveSessionWatch::start(void* hwnd) {
    if (!impl_) return Result::Fail(E_FAIL, "LiveSessionWatch: empty");
    if (impl_->running) return Result::Ok();
    if (!hwnd) return Result::Fail(E_INVALIDARG, "LiveSessionWatch: hwnd");

    impl_->state.hwnd = static_cast<HWND>(hwnd);
    impl_->state.stopping.store(false, std::memory_order_release);
    impl_->state.dirty.store(false, std::memory_order_release);
    impl_->state.posted.store(false, std::memory_order_release);
    impl_->state.cellPosted.store(false, std::memory_order_release);
    impl_->failed = false;
    impl_->errorCode = E_FAIL;
    impl_->error.clear();
    impl_->state.droppedCreates.store(0, std::memory_order_relaxed);
    if (!impl_->preallocateNodes()) {
        impl_->shutdown();
        return Result::Fail(E_OUTOFMEMORY, "LiveSessionWatch: node pool");
    }
    impl_->state.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->state.wake || !impl_->started) {
        const DWORD err = GetLastError();
        impl_->shutdown();
        return Result::Fail(err ? static_cast<long>(err) : static_cast<long>(E_FAIL),
                            "LiveSessionWatch: CreateEvent");
    }

    try {
        impl_->worker = std::thread([self = impl_.get()] { self->threadMain(); });
    } catch (const std::system_error& e) {
        impl_->shutdown();
        return Result::Fail(static_cast<long>(e.code().value()),
                            "LiveSessionWatch: thread");
    }
    WaitForSingleObject(impl_->started, INFINITE);
    if (impl_->failed) {
        const std::string message = impl_->error.empty() ? "LiveSessionWatch: start failed"
                                                         : impl_->error;
        const long code = impl_->errorCode ? impl_->errorCode : static_cast<long>(E_FAIL);
        impl_->shutdown();
        return Result::Fail(code, message);
    }
    impl_->running = true;
    return Result::Ok();
}

void LiveSessionWatch::stop() {
    if (impl_) impl_->shutdown();
}

bool LiveSessionWatch::running() const { return impl_ && impl_->running; }

bool LiveSessionWatch::consumeDirty() noexcept {
    if (!impl_) return false;
    impl_->state.posted.store(false, std::memory_order_release);
    return impl_->state.dirty.exchange(false, std::memory_order_acq_rel);
}

std::vector<LiveSessionCellPatch> LiveSessionWatch::drainCellPatches() {
    std::vector<LiveSessionCellPatch> out;
    if (!impl_) return out;
    impl_->state.cellPosted.store(false, std::memory_order_release);
    std::vector<CellNode*> nodes;
    for (;;) {
        SLIST_ENTRY* entry = InterlockedPopEntrySList(&impl_->state.cells);
        if (!entry) break;
        nodes.push_back(reinterpret_cast<CellNode*>(entry));
    }
    out.reserve(nodes.size());
    for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
        out.push_back(std::move((*it)->patch));
        (*it)->~CellNode();
        _aligned_free(*it);
    }
    return out;
}

}  // namespace wa
