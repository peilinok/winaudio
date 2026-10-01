#include "LiveSessionWatch.h"
#include <atomic>
#include <cstring>
#include <malloc.h>
#include <string>
#include <thread>
#include <vector>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "Log.h"

namespace wa {

constexpr UINT kLiveSessionDirtyMessage = WM_APP + 0x4157;

unsigned liveSessionDirtyMessage() { return kLiveSessionDirtyMessage; }

namespace {

class CreateSink;
class SessionEvents;

struct PendingSession {
    SLIST_ENTRY entry;
    IAudioSessionControl* control;
};

struct WatchState {
    // SLIST_HEADER requires 16-byte alignment and stays the first member.
    SLIST_HEADER pending{};
    std::atomic<bool> stopping{false};
    std::atomic<bool> dirty{false};
    std::atomic<bool> posted{false};
    HWND hwnd = nullptr;
    HANDLE wake = nullptr;

    void postDirty() noexcept {
        if (stopping.load(std::memory_order_acquire)) return;
        dirty.store(true, std::memory_order_release);
        bool expected = false;
        if (posted.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            if (hwnd) PostMessage(hwnd, kLiveSessionDirtyMessage, 0, 0);
        }
    }

    // Lock-free. The caller already AddRef'd control. False means the caller
    // still owns that reference.
    bool enqueue(IAudioSessionControl* control) {
        if (!control || stopping.load(std::memory_order_acquire)) return false;
        void* mem = _aligned_malloc(sizeof(PendingSession), MEMORY_ALLOCATION_ALIGNMENT);
        if (!mem) return false;
        auto* node = static_cast<PendingSession*>(mem);
        std::memset(node, 0, sizeof(*node));
        node->control = control;
        InterlockedPushEntrySList(&pending, &node->entry);
        if (wake) SetEvent(wake);
        return true;
    }
};

class SessionEvents final : public IAudioSessionEvents {
public:
    explicit SessionEvents(WatchState* state) : state_(state) {}

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
    STDMETHODIMP OnSimpleVolumeChanged(float, BOOL, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnChannelVolumeChanged(DWORD, float[], DWORD, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnGroupingParamChanged(LPCGUID, LPCGUID) override { return S_OK; }
    STDMETHODIMP OnStateChanged(AudioSessionState newState) override {
        if (newState == AudioSessionStateExpired && state_) state_->postDirty();
        return S_OK;
    }
    STDMETHODIMP OnSessionDisconnected(AudioSessionDisconnectReason) override {
        if (state_) state_->postDirty();
        return S_OK;
    }

private:
    WatchState* state_ = nullptr;
    LONG refs_ = 1;
};

class CreateSink final : public IAudioSessionNotification {
public:
    explicit CreateSink(WatchState* state) : state_(state) {}

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
        if (!state_->enqueue(created)) {
            created->Release();
            return S_OK;
        }
        state_->postDirty();
        return S_OK;
    }

private:
    WatchState* state_ = nullptr;
    LONG refs_ = 1;
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

    ComPtr<SessionEvents> events;
    ComPtr<CreateSink> createSink;
    std::vector<ComPtr<IAudioSessionManager2>> managers;
    std::vector<IAudioSessionControl*> watched;
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
        running = false;
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

        events.Attach(new SessionEvents(&state));
        createSink.Attach(new CreateSink(&state));
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

        int endpoints = 0;
        int sessions = 0;
        Result r = subscribeFlow(devices.Get(), eCapture, endpoints, sessions);
        if (!r) return r;
        r = subscribeFlow(devices.Get(), eRender, endpoints, sessions);
        if (!r) return r;
        WA_LOG(wa::log::Level::Info, "LiveSessionWatch", "start",
               "endpoints=" + std::to_string(endpoints) + " sessions=" + std::to_string(sessions),
               "ok");
        return Result::Ok();
    }

    Result subscribeFlow(IMMDeviceEnumerator* devices, EDataFlow flow, int& endpoints, int& sessions) {
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
            if (subscribeDevice(dev.Get(), sessions)) ++endpoints;
        }
        return Result::Ok();
    }

    bool subscribeDevice(IMMDevice* dev, int& sessions) {
        LPWSTR id = nullptr;
        HRESULT hr = dev->GetId(&id);
        const std::string idText = (SUCCEEDED(hr) && id) ? narrowAscii(std::wstring(id)) : std::string();
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

        hr = manager->RegisterSessionNotification(createSink.Get());
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "RegisterSessionNotification",
               idText, wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "RegisterSessionNotification",
                   idText, wa::log::hrName(hr));
            return false;
        }
        managers.push_back(manager);

        ComPtr<IAudioSessionEnumerator> sessionEnum;
        hr = manager->GetSessionEnumerator(sessionEnum.GetAddressOf());
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetSessionEnumerator",
               idText, wa::log::hrName(hr));
        if (FAILED(hr) || !sessionEnum) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetSessionEnumerator",
                   idText, wa::log::hrName(hr));
            return true;
        }
        int count = 0;
        hr = sessionEnum->GetCount(&count);
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetSessionCount",
               "n=" + std::to_string(count), wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetSessionCount",
                   "n=" + std::to_string(count), wa::log::hrName(hr));
            return true;
        }

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
            if (registerDropSink(control.Get())) ++sessions;
        }
        return true;
    }

    std::string sessionInstanceId(IAudioSessionControl* control) {
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
            return {};
        }
        LPWSTR id = nullptr;
        hr = control2->GetSessionInstanceIdentifier(&id);
        const std::string text = (SUCCEEDED(hr) && id) ? narrowAscii(std::wstring(id)) : std::string();
        if (id) CoTaskMemFree(id);
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetSessionInstanceIdentifier",
               text.empty() ? "empty" : text, wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetSessionInstanceIdentifier",
                   "empty", wa::log::hrName(hr));
        }
        return text;
    }

    bool registerDropSink(IAudioSessionControl* control) {
        if (!control || !events) return false;
        const std::string instanceId = sessionInstanceId(control);
        if (!instanceId.empty()) {
            for (const auto& seen : watchedIds) {
                if (seen == instanceId) return true;
            }
        }
        AudioSessionState st = AudioSessionStateInactive;
        const HRESULT stateHr = control->GetState(&st);
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "GetState", "", wa::log::hrName(stateHr));
        if (FAILED(stateHr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "GetState", "", wa::log::hrName(stateHr));
        }
        if (SUCCEEDED(stateHr) && st == AudioSessionStateExpired) return false;

        const HRESULT hr = control->RegisterAudioSessionNotification(events.Get());
        WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "RegisterAudioSessionNotification",
               instanceId, wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "RegisterAudioSessionNotification",
                   instanceId, wa::log::hrName(hr));
            return false;
        }
        control->AddRef();
        watched.push_back(control);
        if (!instanceId.empty()) watchedIds.push_back(instanceId);
        return true;
    }

    void drainNewSessions(bool releaseOnly) {
        for (;;) {
            SLIST_ENTRY* entry = InterlockedPopEntrySList(&state.pending);
            if (!entry) break;
            auto* node = reinterpret_cast<PendingSession*>(entry);
            IAudioSessionControl* control = node->control;
            _aligned_free(node);
            if (!control) continue;
            // The queue owns one ref. A new registration AddRefs into watched;
            // an id already seen, an expired row, or releaseOnly does not.
            if (!releaseOnly)
                registerDropSink(control);
            control->Release();
        }
    }

    void releaseSubscriptions() {
        for (auto& manager : managers) {
            if (!manager || !createSink) continue;
            const HRESULT hr = manager->UnregisterSessionNotification(createSink.Get());
            WA_LOG(wa::log::Level::Debug, "LiveSessionWatch", "UnregisterSessionNotification",
                   "", wa::log::hrName(hr));
            if (FAILED(hr)) {
                WA_LOG(wa::log::Level::Warn, "LiveSessionWatch", "UnregisterSessionNotification",
                       "", wa::log::hrName(hr));
            }
        }
        // OnSessionCreated has returned by the time Unregister returns.
        drainNewSessions(true);
        for (IAudioSessionControl* control : watched) {
            if (control && events) {
                const HRESULT hr = control->UnregisterAudioSessionNotification(events.Get());
                WA_LOG(wa::log::Level::Debug, "LiveSessionWatch",
                       "UnregisterAudioSessionNotification", "", wa::log::hrName(hr));
                if (FAILED(hr)) {
                    WA_LOG(wa::log::Level::Warn, "LiveSessionWatch",
                           "UnregisterAudioSessionNotification", "", wa::log::hrName(hr));
                }
            }
            if (control) control->Release();
        }
        watched.clear();
        watchedIds.clear();
        managers.clear();
        events.Reset();
        createSink.Reset();
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
    impl_->failed = false;
    impl_->errorCode = E_FAIL;
    impl_->error.clear();
    InitializeSListHead(&impl_->state.pending);
    impl_->state.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    impl_->started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->state.wake || !impl_->started) {
        const DWORD err = GetLastError();
        impl_->shutdown();
        return Result::Fail(err ? static_cast<long>(err) : static_cast<long>(E_FAIL),
                            "LiveSessionWatch: CreateEvent");
    }

    impl_->worker = std::thread([self = impl_.get()] { self->threadMain(); });
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

}  // namespace wa
