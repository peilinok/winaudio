#include "StartupLaunch.h"
#include "StartupGateHarness.h"
#include "OnDemandAttach.h"
#include "HookModuleIdentity.h"
#include "Log.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <array>
#include <stdexcept>

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
    else std::fprintf(stderr, "Usage: StartupGateProbe --audio | --controller TOKEN CHECKPOINT\n");
    wa::log::shutdown();
    return result;
}
