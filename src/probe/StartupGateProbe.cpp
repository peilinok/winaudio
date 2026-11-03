#include "StartupLaunch.h"
#include "StartupGateHarness.h"
#include "OnDemandAttach.h"
#include "HookModuleIdentity.h"
#include "HookedCallPod.h"
#include "Log.h"
#include "../gui/PipelineAttachUiModel.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <array>
#include <stdexcept>
#include <future>

namespace {
DWORD observe(const char* operation, DWORD error, const std::string& args = {}) {
    WA_LOG(error ? wa::log::Level::Warn : wa::log::Level::Debug, "StartupProbe", operation, args,
           wa::log::hrName(HRESULT_FROM_WIN32(error)));
    return error;
}
#define CHECK_WIN(operation, expression, ...) observe(operation, [&] { \
    const bool ok = (expression); return ok ? DWORD(ERROR_SUCCESS) : GetLastError(); \
}(), __VA_ARGS__)

class Fixture {
public:
    Fixture(const std::wstring& token, bool create) : base(token) {
        if (create) {
            CHECK_WIN("CreateFileMappingW", (map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                sizeof(wa::startup_probe::Shared), (base + L"-map").c_str())) != nullptr, "shared target stages");
            for (size_t i = 0; i < handles.size(); ++i)
                CHECK_WIN("CreateEventW", (handles[i] = CreateEventW(nullptr, TRUE, FALSE, (base + names[i]).c_str())) != nullptr,
                          "manual reset " + std::to_string(i));
        } else {
            CHECK_WIN("OpenFileMappingW", (map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, (base + L"-map").c_str())) != nullptr,
                      "FILE_MAP_ALL_ACCESS");
            for (size_t i = 0; i < handles.size(); ++i)
                CHECK_WIN("OpenEventW", (handles[i] = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                          (base + names[i]).c_str())) != nullptr, "SYNCHRONIZE|EVENT_MODIFY_STATE " + std::to_string(i));
        }
        if (map) CHECK_WIN("MapViewOfFile", (shared = static_cast<wa::startup_probe::Shared*>(
            MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0))) != nullptr, "shared target stages");
    }
    ~Fixture() {
        if (shared) CHECK_WIN("UnmapViewOfFile", UnmapViewOfFile(shared) != FALSE, "stage mapping");
        for (HANDLE h : handles) if (h) CHECK_WIN("CloseHandle", CloseHandle(h) != FALSE, "stage event");
        if (map) CHECK_WIN("CloseHandle", CloseHandle(map) != FALSE, "stage mapping");
    }
    bool valid() const {
        if (!shared) return false;
        for (HANDLE h : handles) if (!h) return false;
        return true;
    }
    void signal(size_t i) { CHECK_WIN("SetEvent", SetEvent(handles[i]) != FALSE, "index=" + std::to_string(i)); }
    DWORD wait(size_t i, DWORD ms) {
        const DWORD value = WaitForSingleObject(handles[i], ms);
        const DWORD error = value == WAIT_OBJECT_0 ? ERROR_SUCCESS : value == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
        observe("WaitForSingleObject", error, "index=" + std::to_string(i) + " outcome=" + std::to_string(value));
        return value;
    }
    std::wstring base;
    HANDLE map = nullptr;
    std::array<HANDLE, 5> handles{};
    wa::startup_probe::Shared* shared = nullptr;
    static constexpr const wchar_t* names[] = {L"-ready", L"-stop", L"-checkpoint", L"-release", L"-exit"};
};

int run(const std::wstring& token, int checkpoint, bool audio, bool controller, bool releaseEarly = false) {
    Fixture fixture(token, !controller);
    if (!fixture.valid()) return 2;
    wchar_t self[MAX_PATH]{};
    if (CHECK_WIN("GetModuleFileNameW", GetModuleFileNameW(nullptr, self, MAX_PATH) != 0, "current executable")) return 3;
    std::wstring folder(self);
    folder.resize(folder.find_last_of(L"\\/") + 1);
    wa::StartupRequest request;
    request.executable = folder + L"StartupGateTarget.exe";
    request.arguments = L"--token=" + token + (audio ? L" --audio" : L"");
    if (releaseEarly) request.arguments += L" --release-before-main";
    wa::OnDemandAttach attachment;
    uint32_t heldCrt = 0xFFFFFFFFu;
    const auto out = wa::launchAtEntry(request, [&](uint32_t pid, const std::atomic_bool& cancelled) {
        heldCrt = fixture.shared->crtEntered;
        if (heldCrt || fixture.shared->mainEntered)
            return wa::Result::Fail(static_cast<long>(E_FAIL), "target crossed entry before readiness");
        // The no-audio controller exercises only entry ownership. The manual
        // audio mode uses the same production attach/receiver as WinAudio.
        return audio ? attachment.start(pid, {}, wa::PipelineFlow::Render, &cancelled) : wa::Result::Ok();
    }, nullptr, [&](wa::StartupStage stage, const wa::StartupOutcome& snapshot) {
        fixture.shared->targetPid = snapshot.pid;
        const size_t i = static_cast<size_t>(stage);
        LARGE_INTEGER counter{};
        CHECK_WIN("QueryPerformanceCounter(stage)", QueryPerformanceCounter(&counter) != FALSE, "index=" + std::to_string(i));
        fixture.shared->stageCounters[i] = counter.QuadPart;
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&fixture.shared->controllerStage), static_cast<LONG>(i + 1));
        std::printf("stage=%s pid=%u qpc=%lld restored=%d advanced=%d detached=%d committed=%d\n",
                    wa::startupStageName(stage), snapshot.pid, counter.QuadPart, snapshot.entryRestored,
                    snapshot.entryAdvanced, snapshot.debugDetached, snapshot.committed);
        std::fflush(stdout);
        if (checkpoint == static_cast<int>(i)) {
            fixture.signal(2);
            if (fixture.wait(3, 15000) != WAIT_OBJECT_0) throw std::runtime_error("checkpoint release timed out");
        }
    });
    std::printf("startup result=%d hr=0x%08lX pid=%u created=%llu base=0x%llX rva=0x%lX entry=0x%llX byte=0x%02X "
                "restored=%d advanced=%d detached=%d committed=%d exit_confirmed=%d message=%s\n",
                out.result.ok, out.result.code, out.pid, out.creationTime, static_cast<unsigned long long>(out.imageBase),
                static_cast<unsigned long>(out.entryRva), static_cast<unsigned long long>(out.entryAddress), out.originalByte,
                out.entryRestored, out.entryAdvanced, out.debugDetached, out.committed, out.targetExitConfirmed, out.result.message.c_str());
    if (!out.result) return 4;
    if (fixture.wait(0, 10000) != WAIT_OBJECT_0) return 5;
    bool passed = heldCrt == 0 && fixture.shared->crtEntered == 1 && fixture.shared->mainEntered == 1 &&
        out.imageBase == fixture.shared->imageBase && out.entryRva == fixture.shared->entryRva &&
        out.originalByte == fixture.shared->entryByte && out.entryAdvanced && out.debugDetached && out.committed;
    if (audio) {
        const auto calls = attachment.drain();
        const uint32_t sourcePid = attachment.pid();
        unsigned matched = 0;
        size_t next = 0;
        uint32_t clientStream = 0;
        bool activateOutput = false;
        bool initializeArguments = false;
        const std::string expectedInit = "ShareMode=SHARED StreamFlags=0x0 hnsBufferDuration=1000000 hnsPeriodicity=0 fmt=" +
            std::to_string(fixture.shared->sampleRate) + "/" + std::to_string(fixture.shared->bits) + "/" +
            std::to_string(fixture.shared->channels);
        const char* requiredMethods[] = {"Activate", "Initialize", "Start", "Stop"};
        for (const auto& call : calls) {
            std::printf("record source_pid=%u stream=%u method=%s hr=0x%08lX args=%s\n", sourcePid, call.streamId,
                        call.method.c_str(), static_cast<long>(call.hresult), call.args.c_str());
            if (call.method == "Activate" && call.iface == "IMMDevice" && call.args == "iid=IAudioClient") {
                for (const auto& parameter : call.parameters)
                    if (parameter.name == "ppInterface") activateOutput = _strtoui64(parameter.post.c_str(), nullptr, 0) ==
                        fixture.shared->clientAddress;
            }
            if (call.method == "Initialize" && call.iface == "IAudioClient") initializeArguments = call.args == expectedInit;
            if (next < 4 && call.method == requiredMethods[next] && call.hresult == fixture.shared->sourceResults[next] &&
                call.iface == (next == 0 ? "IMMDevice" : "IAudioClient")) {
                if (next == 1) clientStream = call.streamId;
                if (next > 1 && call.streamId != clientStream) passed = false;
                ++next;
            }
        }
        for (const auto* required : requiredMethods) {
            unsigned count = 0;
            for (const auto& call : calls) if (sourcePid == out.pid && call.method == required) {
                std::printf("call method=%s source_pid=%u hr=0x%08lX args=%s\n", call.method.c_str(), sourcePid,
                            static_cast<long>(call.hresult), call.args.c_str());
                if (call.hresult == S_OK) ++count;
            }
            if (count == 1) ++matched;
        }
        std::wstring expected;
        bool loaded = false;
        const auto staged = wa::stageHookModule(folder + L"WinAudioHook.dll", expected);
        const auto identity = staged ? wa::inspectHookModules(out.pid, expected, loaded) : staged;
        const auto readyCounter = fixture.shared->stageCounters[static_cast<size_t>(wa::StartupStage::ReceiverReady)];
        passed = passed && next == 4 && sourcePid == out.pid && fixture.shared->sourceThread == out.mainThreadId &&
            activateOutput && initializeArguments && calls.size() == (releaseEarly ? 5u : 4u) &&
            clientStream == static_cast<uint32_t>(fixture.shared->clientAddress) &&
            fixture.shared->calls == 4 && SUCCEEDED(fixture.shared->audioResult) && matched == 4 && identity && loaded &&
            fixture.shared->firstCallCounter > readyCounter && fixture.shared->crtCounter > readyCounter;
        std::printf("source_tid=%u source_client=0x%llX sequence_matched=%zu release_early=%d received_rows=%zu\n",
                    fixture.shared->sourceThread, static_cast<unsigned long long>(fixture.shared->clientAddress), next,
                    releaseEarly, calls.size());
        if (releaseEarly) {
            passed = passed && fixture.shared->releaseRowsBefore == 4 && fixture.shared->releaseRowsAfter == 5 &&
                fixture.shared->uninitRowsAfter == 5 && calls.back().method == "Stop" && calls.back().hresult == S_FALSE;
            std::printf("cleanup_rows before_Release=%u after_Release=%u after_CoUninitialize=%u\n",
                        fixture.shared->releaseRowsBefore, fixture.shared->releaseRowsAfter, fixture.shared->uninitRowsAfter);
        }
        std::printf("audio source_calls=%u matching_methods=%u receiver_qpc=%lld crt_qpc=%lld first_call_qpc=%lld "
                    "identity=%d expected=%ls coverage=discovered-implementations-only\n", fixture.shared->calls, matched,
                    readyCounter, fixture.shared->crtCounter, fixture.shared->firstCallCounter, identity.ok && loaded, expected.c_str());
    }
    attachment.stop();
    if (!controller) {
        HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, out.pid);
        const DWORD openError = process ? ERROR_SUCCESS : GetLastError();
        observe("OpenProcess(final child)", openError, "pid=" + std::to_string(out.pid));
        fixture.signal(1);
        DWORD code = STILL_ACTIVE;
        if (!process) passed = false;
        else {
            const DWORD waited = WaitForSingleObject(process, 5000);
            passed = passed && waited == WAIT_OBJECT_0;
            CHECK_WIN("GetExitCodeProcess", GetExitCodeProcess(process, &code) != FALSE, "final child");
            passed = passed && code == 0;
            CHECK_WIN("CloseHandle", CloseHandle(process) != FALSE, "final child");
        }
        std::printf("target_exit=%lu\n", code);
    }
    std::printf("result=%s mode=%s\n", passed ? "PASS" : "FAIL", audio ? "real-hook" : "entry-only");
    return passed ? 0 : 6;
}

int runProduct(uint32_t methods, bool boundaries = false) {
    Fixture fixture(L"Local\\WinAudio-StartupProduct-" + std::to_wstring(GetCurrentProcessId()), true);
    if (!fixture.valid()) return 2;
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring folder(self);
    folder.resize(folder.find_last_of(L"\\/") + 1);
    wa::StartupRequest request;
    request.executable = folder + L"StartupGateTarget.exe";
    request.arguments = L"--token=" + fixture.base + L" --audio --audio-all-stages";
    if (boundaries) request.arguments += L" --late-system-dll";
    wa::PipelineAttachController controller;
    wa::CallLogStore store;
    wa::pipeline_attach_ui::SegmentSelection selection;
    const auto accepted = controller.startStartup(request, methods);
    if (!accepted) return 3;
    const auto deadline = GetTickCount64() + 15000;
    while (controller.startupStatus().running && GetTickCount64() < deadline) Sleep(1);
    const auto status = controller.startupStatus();
    HANDLE process = status.outcome.pid ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE,
        FALSE, status.outcome.pid) : nullptr;
    bool passed = !status.running && status.outcome.result && status.outcome.committed && process;
    passed = fixture.wait(0, 10000) == WAIT_OBJECT_0 && passed;
    std::unique_ptr<Fixture> unrelated;
    PROCESS_INFORMATION other{};
    if (boundaries) {
        unrelated = std::make_unique<Fixture>(fixture.base + L"-other", true);
        std::wstring command = L"\"" + request.executable + L"\" --token=" + unrelated->base +
            L" --audio --audio-all-stages";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        const DWORD error = CHECK_WIN("CreateProcessW(unobserved target)",
            CreateProcessW(request.executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, folder.c_str(), &startup, &other) != FALSE, "explicit unobserved PID; no injection");
        passed = !error && unrelated->valid() && passed;
        if (other.hProcess) {
            CHECK_WIN("CloseHandle(unobserved main thread)", CloseHandle(other.hThread) != FALSE, "created target");
            passed = unrelated->wait(0, 10000) == WAIT_OBJECT_0 && passed;
            FILETIME created{}, exited{}, kernel{}, user{};
            const auto timeError = CHECK_WIN("GetProcessTimes(unobserved target)",
                GetProcessTimes(other.hProcess, &created, &exited, &kernel, &user) != FALSE, "actual process identity");
            const uint64_t createdTime = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            passed = !timeError && createdTime && other.dwProcessId != status.outcome.pid &&
                unrelated->shared->calls == 12 && SUCCEEDED(unrelated->shared->audioResult) && passed;
            std::printf("unobserved pid=%lu created=%llu source_calls=%u\n", other.dwProcessId,
                createdTime, unrelated->shared->calls);
        }
    }
    // Deliberately delay GUI delivery after the target's entire known sequence.
    // Its receiving cursor was established at the held entry, not at this poll.
    Sleep(250);
    for (auto& batch : controller.poll()) selection.apply(store, std::move(batch));
    const auto id = store.activeAttach(status.outcome.pid);
    const auto* segment = store.segment(id);
    passed = passed && segment && segment->startup && segment->startup->receiverReady &&
        segment->instance.created == status.outcome.creationTime && segment->methods == methods;
    const auto view = store.view();
    size_t position = 0;
    const char* names[] = {"Activate", "Initialize", "Start", "Stop"};
    for (unsigned stage = 0; stage < 3; ++stage) {
        const auto& source = fixture.shared->audioStages[stage];
        passed = passed && source.calls == 4 && source.client && source.thread && source.counter;
        std::printf("source stage=%u pid=%u tid=%u qpc=%lld client=0x%llX calls=%u\n", stage, status.outcome.pid,
            source.thread, source.counter, static_cast<unsigned long long>(source.client), source.calls);
        for (unsigned method = 0; method < 4; ++method) {
            if (!(methods & (1u << method))) continue;
            const auto* record = view.at(position++);
            if (!record) { passed = false; continue; }
            const auto& call = record->call;
            std::printf("received stage=%u sequence=%llu pid=%u method=%s hr=0x%08lX args=%s\n", stage,
                static_cast<unsigned long long>(record->id.sequence), call.pid, call.method.c_str(),
                static_cast<long>(call.hresult), call.args.c_str());
            passed = passed && call.pid == status.outcome.pid && call.processCreated == status.outcome.creationTime &&
                call.observationGeneration == segment->observationGeneration && call.method == names[method] &&
                call.hresult == source.results[method] && SUCCEEDED(source.results[method]);
            if (method == 0) {
                bool found = false;
                for (const auto& parameter : call.parameters)
                    if (parameter.name == "ppInterface") found = _strtoui64(parameter.post.c_str(), nullptr, 0) == source.client;
                passed = passed && found && call.args == "iid=IAudioClient";
            } else {
                passed = passed && call.streamId == static_cast<uint32_t>(source.client);
                if (method == 1) {
                    const auto expected = "ShareMode=SHARED StreamFlags=0x0 hnsBufferDuration=1000000 hnsPeriodicity=0 fmt=" +
                        std::to_string(source.sampleRate) + "/" + std::to_string(source.bits) + "/" + std::to_string(source.channels);
                    passed = passed && call.args == expected;
                }
            }
        }
    }
    passed = passed && view.size() == position && fixture.shared->calls == 12 && fixture.shared->tlsEntered == 1 &&
        fixture.shared->tlsCounter < fixture.shared->crtCounter && fixture.shared->audioStages[0].thread == status.outcome.mainThreadId &&
        fixture.shared->audioStages[1].thread == status.outcome.mainThreadId &&
        fixture.shared->audioStages[2].thread != status.outcome.mainThreadId;
    if (segment && segment->startup) passed = passed && fixture.shared->tlsCounter < segment->startup->receiverReadyCounter &&
        segment->startup->receiverReadyCounter < fixture.shared->crtCounter &&
        segment->startup->receiverReadyCounter < fixture.shared->audioStages[0].counter;
    std::wstring hook;
    bool loaded = false;
    const auto staged = wa::OnDemandAttach::freezeHook(hook);
    const auto identity = staged ? wa::inspectHookModules(status.outcome.pid, hook, loaded) : staged;
    passed = passed && identity && loaded;
    if (boundaries) {
        bool otherLoaded = true;
        const auto otherIdentity = other.hProcess ? wa::inspectHookModules(other.dwProcessId, hook, otherLoaded)
            : wa::Result::Fail(static_cast<long>(E_FAIL), "unobserved target missing");
        passed = passed && otherIdentity && !otherLoaded && store.segments(true).size() == 1 &&
            fixture.shared->lateDllWasAbsent == 1 && fixture.shared->lateDllLoaded == 1 &&
            fixture.shared->lateDllCounter > fixture.shared->mainCounter &&
            fixture.shared->audioStages[1].counter > fixture.shared->lateDllCounter;
        std::printf("boundaries observed_pid=%u unobserved_pid=%lu other_hook_loaded=%d late_dll=%u "
            "known_rows=%zu unknown_implementation_scope=unit-tested-refusal\n", status.outcome.pid,
            other.dwProcessId, otherLoaded, fixture.shared->lateDllLoaded, view.size());
    }
    std::printf("product pid=%u created=%llu methods=%u rows=%zu expected=%zu entry=0x%llX hook=%ls identity=%d\n",
        status.outcome.pid, status.outcome.creationTime, methods, view.size(), position,
        static_cast<unsigned long long>(status.outcome.entryAddress), hook.c_str(), identity.ok && loaded);
    if (segment) for (const auto& line : wa::CallLogStore::exportSummary(store.freezeExport(id, 1), false))
        std::printf("export %s\n", line.c_str());
    controller.shutdown();
    passed = passed && process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    if (other.hProcess) {
        passed = WaitForSingleObject(other.hProcess, 0) == WAIT_TIMEOUT && passed;
        unrelated->signal(1);
        const auto waited = WaitForSingleObject(other.hProcess, 5000);
        DWORD otherExit = STILL_ACTIVE;
        const auto exitError = CHECK_WIN("GetExitCodeProcess(unobserved target)",
            GetExitCodeProcess(other.hProcess, &otherExit) != FALSE, "final unobserved target exit");
        passed = waited == WAIT_OBJECT_0 && !exitError && otherExit == 0 && passed;
        if (waited != WAIT_OBJECT_0) {
            CHECK_WIN("TerminateProcess(unobserved fixture cleanup)", TerminateProcess(other.hProcess, 91) != FALSE,
                "failed probe cleanup only");
            WaitForSingleObject(other.hProcess, 5000);
        }
        CHECK_WIN("CloseHandle(unobserved target)", CloseHandle(other.hProcess) != FALSE, "final target handle");
        std::printf("unobserved target_exit=%lu\n", otherExit);
    }
    fixture.signal(1);
    DWORD code = STILL_ACTIVE;
    if (process) {
        const auto waited = WaitForSingleObject(process, 5000);
        GetExitCodeProcess(process, &code);
        passed = passed && waited == WAIT_OBJECT_0 && code == 0;
        if (waited != WAIT_OBJECT_0) { TerminateProcess(process, 91); WaitForSingleObject(process, 5000); }
        CloseHandle(process);
    }
    std::printf("product target_exit=%lu result=%s\n", code, passed ? "PASS" : "FAIL");
    return passed ? 0 : 7;
}

int fixtureOwner(const std::wstring& token) {
    Fixture fixture(token, true);
    HANDLE gate = CreateEventW(nullptr, TRUE, FALSE, (token + L"-gate").c_str());
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, (token + L"-entered").c_str());
    if (!fixture.valid() || !gate || !entered) return 2;
    std::printf("fixture token=%ls ready=1 owner_pid=%lu\n", token.c_str(), GetCurrentProcessId());
    std::fflush(stdout);
    HANDLE target = nullptr;
    const auto deadline = GetTickCount64() + 600000;
    while (WaitForSingleObject(fixture.handles[1], 1000) == WAIT_TIMEOUT && GetTickCount64() < deadline) {
        if (!target && fixture.shared->pid) {
            target = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, fixture.shared->pid);
            FILETIME created{}, exited{}, kernel{}, user{};
            if (target && GetProcessTimes(target, &created, &exited, &kernel, &user))
                std::printf("fixture target_pid=%u created=%llu\n", fixture.shared->pid,
                    (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime);
        }
        std::printf("fixture pid=%u tls=%u crt=%u main=%u source_calls=%u held=%d\n", fixture.shared->pid,
            fixture.shared->tlsEntered, fixture.shared->crtEntered, fixture.shared->mainEntered, fixture.shared->calls,
            WaitForSingleObject(entered, 0) == WAIT_OBJECT_0 && WaitForSingleObject(gate, 0) == WAIT_TIMEOUT);
        std::fflush(stdout);
    }
    SetEvent(gate);
    fixture.signal(1);
    DWORD code = STILL_ACTIVE;
    bool passed = true;
    if (target) {
        const auto waited = WaitForSingleObject(target, 5000);
        GetExitCodeProcess(target, &code);
        passed = waited == WAIT_OBJECT_0;
        CloseHandle(target);
    }
    std::printf("fixture target_pid=%u target_exit=%lu confirmed=%d source_calls=%u\n",
        fixture.shared->pid, code, passed, fixture.shared->calls);
    CloseHandle(gate);
    CloseHandle(entered);
    return passed ? 0 : 3;
}

int runDelayedReadiness() {
    Fixture fixture(L"Local\\WinAudio-DelayedStartup-" + std::to_wstring(GetCurrentProcessId()), true);
    if (!fixture.valid()) return 2;
    HANDLE configReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE configRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE receiverReady = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE receiverRelease = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!configReady || !configRelease || !receiverReady || !receiverRelease) return 3;
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring folder(self);
    folder.resize(folder.find_last_of(L"\\/") + 1);
    wa::StartupRequest request;
    request.executable = folder + L"StartupGateTarget.exe";
    request.arguments = L"--token=" + fixture.base + L" --audio";
    wa::OnDemandAttach attach;
    std::atomic<uint32_t> pid{0};
    auto work = std::async(std::launch::async, [&] {
        return wa::launchAtEntry(request, [&](uint32_t target, const std::atomic_bool& cancel) {
            pid.store(target);
            return attach.start(target, {}, wa::PipelineFlow::Render, &cancel, {},
                wa::methodBit(wa::MonitorMethod::Initialize), {}, [&](wa::OnDemandAttach::Stage stage) {
                    if (stage == wa::OnDemandAttach::Stage::Installing) {
                        fixture.signal(2);
                        if (fixture.wait(3, 5000) != WAIT_OBJECT_0) throw std::runtime_error("install delay release failed");
                    } else if (stage == wa::OnDemandAttach::Stage::Receiving) {
                        SetEvent(configReady);
                        if (WaitForSingleObject(configRelease, 5000) != WAIT_OBJECT_0) throw std::runtime_error("configuration delay release failed");
                    }
                });
        }, nullptr, [&](wa::StartupStage stage, const wa::StartupOutcome&) {
            if (stage == wa::StartupStage::ReceiverReady) {
                SetEvent(receiverReady);
                if (WaitForSingleObject(receiverRelease, 5000) != WAIT_OBJECT_0) throw std::runtime_error("receiver delay release failed");
            }
        });
    });
    bool passed = fixture.wait(2, 5000) == WAIT_OBJECT_0;
    wchar_t mapName[64]{};
    wa::hook_ipc::mapName(pid.load(), mapName, 64);
    // monitorState uses an interlocked compare-exchange even for a read.
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapName);
    auto* ring = mapping ? static_cast<const wa::hook_ipc::Ring*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
    const auto check = [&](const char* stage, bool receiving) {
        const auto state = ring ? wa::hook_ipc::monitorState(*ring) : UINT64_MAX;
        const uint32_t mask = static_cast<uint32_t>(state), generation = static_cast<uint32_t>(state >> 32);
        const bool accepted = ring && fixture.shared->crtEntered == 0 && fixture.shared->mainEntered == 0 &&
            (receiving ? mask == wa::methodBit(wa::MonitorMethod::Initialize) && generation == 1 && ring->captureEnabled == 1
                       : mask == 0 && generation == 0 && ring->captureEnabled == 0);
        std::printf("delay stage=%s pid=%u crt=%u main=%u methods=%u generation=%u capture=%u accepted=%d\n",
            stage, pid.load(), fixture.shared->crtEntered, fixture.shared->mainEntered, mask, generation,
            ring ? ring->captureEnabled : UINT32_MAX, accepted);
        passed = accepted && passed;
    };
    check("installation", false);
    fixture.signal(3);
    passed = WaitForSingleObject(configReady, 5000) == WAIT_OBJECT_0 && passed;
    check("initial-configuration", false);
    SetEvent(configRelease);
    passed = WaitForSingleObject(receiverReady, 5000) == WAIT_OBJECT_0 && passed;
    check("receiver-handoff", true);
    SetEvent(receiverRelease);
    const auto out = work.get();
    passed = out.result && out.committed && passed;
    passed = fixture.wait(0, 5000) == WAIT_OBJECT_0 && passed;
    const auto rows = attach.drain();
    passed = rows.size() == 1 && rows[0].method == "Initialize" && rows[0].hresult == fixture.shared->sourceResults[1] &&
        fixture.shared->calls == 4 && passed;
    HANDLE process = out.pid ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, out.pid) : nullptr;
    attach.stop();
    fixture.signal(1);
    DWORD code = STILL_ACTIVE;
    if (process) {
        passed = WaitForSingleObject(process, 5000) == WAIT_OBJECT_0 && passed;
        GetExitCodeProcess(process, &code);
        passed = code == 0 && passed;
        CloseHandle(process);
    } else passed = false;
    if (ring) UnmapViewOfFile(ring);
    if (mapping) CloseHandle(mapping);
    for (HANDLE event : {configReady, configRelease, receiverReady, receiverRelease}) CloseHandle(event);
    std::printf("delay result=%s received=%zu source_calls=%u target_exit=%lu\n", passed ? "PASS" : "FAIL", rows.size(), fixture.shared->calls, code);
    return passed ? 0 : 4;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    wa::log::init();
    wa::log::addStderrSink();
    wa::log::setLevel(wa::log::Level::Debug);
    int result = 1;
    if (argc == 4 && std::wstring(argv[1]) == L"--controller")
        result = run(argv[2], _wtoi(argv[3]), false, true);
    else if (argc == 2 && std::wstring(argv[1]) == L"--audio")
        result = run(L"Local\\WinAudio-StartupAudio-" + std::to_wstring(GetCurrentProcessId()), -1, true, false);
    else if (argc == 2 && std::wstring(argv[1]) == L"--audio-release")
        result = run(L"Local\\WinAudio-StartupAudio-" + std::to_wstring(GetCurrentProcessId()), -1, true, false, true);
    else if (argc == 2 && std::wstring(argv[1]) == L"--product") result = runProduct(wa::kDefaultMonitorMethods);
    else if (argc == 2 && std::wstring(argv[1]) == L"--product-off") result = runProduct(0);
    else if (argc == 2 && std::wstring(argv[1]) == L"--product-initialize") result = runProduct(wa::methodBit(wa::MonitorMethod::Initialize));
    else if (argc == 2 && std::wstring(argv[1]) == L"--product-boundaries") result = runProduct(wa::kDefaultMonitorMethods, true);
    else if (argc == 3 && std::wstring(argv[1]) == L"--fixture") result = fixtureOwner(argv[2]);
    else if (argc == 2 && std::wstring(argv[1]) == L"--delayed-readiness") result = runDelayedReadiness();
    else std::fprintf(stderr, "Usage: StartupGateProbe --audio | --controller TOKEN CHECKPOINT\n");
    wa::log::shutdown();
    return result;
}
