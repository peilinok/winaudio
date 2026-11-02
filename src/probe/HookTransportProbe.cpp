// Synthetic cross-process transport evidence; no injection or audio is used.
#include "HookTransportReader.h"
#include "Log.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace ipc = wa::hook_ipc;
constexpr uint32_t kWriters = 4;
constexpr uint32_t kWrites = 4096;
constexpr uint32_t kAttempted = kWriters * kWrites;
constexpr uint32_t kGeneration = 1;

bool observed(const char* call, bool success, const std::string& args,
              wa::log::Level level = wa::log::Level::Debug) {
    const DWORD error = success ? ERROR_SUCCESS : GetLastError();
    WA_LOG(success ? level : wa::log::Level::Warn, "HookTransportProbe", call, args,
           wa::log::hrName(HRESULT_FROM_WIN32(error)));
    return success;
}

void closeHandle(HANDLE handle) {
    if (handle) observed("CloseHandle", CloseHandle(handle) != FALSE,
                         "handle=" + std::to_string(reinterpret_cast<uintptr_t>(handle)));
}

DWORD waitForChild(HANDLE process, DWORD timeout) {
    const DWORD outcome = WaitForSingleObject(process, timeout);
    const DWORD error = outcome == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS;
    WA_LOG(outcome == WAIT_FAILED ? wa::log::Level::Warn : wa::log::Level::Trace,
           "HookTransportProbe", "WaitForSingleObject",
           "handle=" + std::to_string(reinterpret_cast<uintptr_t>(process)) + " timeout=" + std::to_string(timeout),
           "outcome=" + std::to_string(outcome) + " " + wa::log::hrName(HRESULT_FROM_WIN32(error)));
    return outcome;
}

bool countersZero(const ipc::LaneCounters& counters) {
    return ipc::atomicRead64(counters.reserved) == 0 && ipc::atomicRead64(counters.overwritten) == 0 &&
        ipc::atomicRead64(counters.rejected) == 0 && ipc::atomicRead64(counters.unknown) == 0;
}

bool creationTime(HANDLE process, uint64_t& created) {
    FILETIME creation{}, exit{}, kernel{}, user{};
    if (!observed("GetProcessTimes", GetProcessTimes(process, &creation, &exit, &kernel, &user) != FALSE,
                  "handle=" + std::to_string(reinterpret_cast<uintptr_t>(process)))) return false;
    created = (static_cast<uint64_t>(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
    return created != 0;
}

// Fill each complete text buffer to catch partial copies.
template <size_t N>
void textPattern(char (&out)[N], uint32_t seed, uint32_t salt) {
    for (size_t i = 0; i + 1 < N; ++i)
        out[i] = static_cast<char>('!' + (seed + salt + static_cast<uint32_t>(i) * 17u) % 90u);
    out[N - 1] = 0;
}

ipc::CallPod produce(DWORD pid, uint64_t created, uint32_t writer, uint32_t row, bool pump) {
    ipc::CallPod pod{};
    const uint32_t id = writer * kWrites * 2 + row * 2 + (pump ? 1u : 0u) + 1;
    const uint32_t seed = id ^ pid ^ static_cast<uint32_t>(created);
    pod.streamId = id;
    pod.timeMs = static_cast<int64_t>((static_cast<uint64_t>(pid) << 32) | id);
    textPattern(pod.iface, seed, 1);
    textPattern(pod.method, seed, 2);
    textPattern(pod.args, seed, 3);
    pod.hresult = static_cast<int32_t>(0x80000000u | seed);
    pod.pump = pump ? 1 : 0;
    pod.xrun = 0;
    pod.hasCategory = static_cast<uint8_t>((id >> 0) & 1u);
    pod.hasRaw = static_cast<uint8_t>((id >> 1) & 1u);
    pod.hasMatchFormat = static_cast<uint8_t>((id >> 2) & 1u);
    pod.hasExclusive = static_cast<uint8_t>((id >> 3) & 1u);
    pod.hasFormat = static_cast<uint8_t>((id >> 4) & 1u);
    pod.raw = static_cast<uint8_t>((id >> 5) & 1u);
    pod.matchFormat = static_cast<uint8_t>((id >> 6) & 1u);
    pod.exclusive = static_cast<uint8_t>((id >> 7) & 1u);
    textPattern(pod.category, seed, 4);
    textPattern(pod.format, seed, 5);
    for (uint32_t i = 0; i < ipc::kArgBlobCap; ++i)
        pod.argBlob[i] = static_cast<uint8_t>((seed + i * 29u) & 255u);
    char pre[128]{}, post[128]{};
    sprintf_s(pre, "pid=%lu;created=%llu;writer=%u;row=%u;lane=%u", pid, created, writer, row, pump ? 1u : 0u);
    sprintf_s(post, "id=%u;seed=%u;generation=%u", id, seed, kGeneration);
    ipc::ArgBlobWriter blob;
    blob.reset(pod.argBlob, static_cast<uint16_t>(ipc::kArgBlobCap));
    blob.push(2, ipc::kArgInOut, "SyntheticIdentity", "identity", pre, post);
    blob.push(1, ipc::kArgRet, "HRESULT", "result", "before", "after");
    pod.argBlobLen = blob.len;
    pod.observationGeneration = kGeneration;
    return pod;
}

// This oracle checks individual fields and decodes the blob. It never calls
// produce() or compares a received object with another producer-built object.
template <size_t N>
bool checkText(const char (&text)[N], uint32_t seed, uint32_t salt) {
    if (text[N - 1] != 0) return false;
    for (size_t i = 0; i < N - 1; ++i)
        if (static_cast<uint32_t>(static_cast<unsigned char>(text[i])) !=
            33u + (seed + salt + static_cast<uint32_t>(i) * 17u) % 90u) return false;
    return true;
}

bool validate(const ipc::CallPod& pod, DWORD pid, uint64_t created, bool pump, uint32_t& writer) {
    if (!pod.streamId || pod.streamId > kAttempted * 2) return false;
    const uint32_t ordinal = pod.streamId - 1;
    writer = ordinal / (kWrites * 2);
    const uint32_t row = (ordinal % (kWrites * 2)) / 2;
    const uint32_t seed = pod.streamId ^ pid ^ static_cast<uint32_t>(created);
    if ((ordinal & 1u) != (pump ? 1u : 0u) || pod.pump != (pump ? 1 : 0) ||
        pod.timeMs != static_cast<int64_t>((static_cast<uint64_t>(pid) << 32) | pod.streamId) ||
        static_cast<uint32_t>(pod.hresult) != (0x80000000u | seed) || pod.xrun != 0 ||
        pod.observationGeneration != kGeneration ||
        !checkText(pod.iface, seed, 1) || !checkText(pod.method, seed, 2) ||
        !checkText(pod.args, seed, 3) || !checkText(pod.category, seed, 4) ||
        !checkText(pod.format, seed, 5)) return false;
    const uint8_t flags[] = {pod.hasCategory, pod.hasRaw, pod.hasMatchFormat, pod.hasExclusive,
                            pod.hasFormat, pod.raw, pod.matchFormat, pod.exclusive};
    for (unsigned bit = 0; bit < 8; ++bit)
        if (flags[bit] != ((pod.streamId >> bit) & 1u)) return false;
    if (pod.argBlobLen > ipc::kArgBlobCap || pod.argBlobLen < 2 || pod.argBlob[1] != 2) return false;
    const std::string expectedPre = "pid=" + std::to_string(pid) + ";created=" + std::to_string(created) +
        ";writer=" + std::to_string(writer) + ";row=" + std::to_string(row) + ";lane=" + (pump ? "1" : "0");
    const std::string expectedPost = "id=" + std::to_string(pod.streamId) + ";seed=" + std::to_string(seed) +
        ";generation=" + std::to_string(kGeneration);
    bool good = true;
    unsigned fields = 0;
    size_t bytes = 2;
    const bool decoded = ipc::decodeArgBlob(pod.argBlob, pod.argBlobLen,
        [&](uint8_t depth, uint8_t dir, const char* type, uint8_t tl, const char* name, uint8_t nl,
            const char* pre, uint8_t pl, const char* post, uint8_t ql) {
            const bool identity = fields == 0;
            good = good && fields < 2 && depth == (identity ? 2 : 1) &&
                dir == (identity ? ipc::kArgInOut : ipc::kArgRet) &&
                std::string(type, tl) == (identity ? "SyntheticIdentity" : "HRESULT") &&
                std::string(name, nl) == (identity ? "identity" : "result") &&
                std::string(pre, pl) == (identity ? expectedPre : "before") &&
                std::string(post, ql) == (identity ? expectedPost : "after");
            bytes += 6u + tl + nl + pl + ql;
            ++fields;
        });
    if (!decoded || !good || fields != 2 || bytes != pod.argBlobLen) return false;
    for (uint32_t i = pod.argBlobLen; i < ipc::kArgBlobCap; ++i)
        if (pod.argBlob[i] != ((seed + i * 29u) & 255u)) return false;
    return true;
}

int child() {
    const DWORD pid = GetCurrentProcessId();
    uint64_t created = 0;
    if (!creationTime(GetCurrentProcess(), created)) return 10;
    wchar_t name[64]{};
    ipc::mapName(pid, name, 64);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!observed("OpenFileMappingW", mapping != nullptr, "pid=" + std::to_string(pid))) return 11;
    auto* ring = static_cast<ipc::Ring*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::Ring)));
    const bool mapped = observed("MapViewOfFile/child", ring != nullptr, "bytes=" + std::to_string(sizeof(ipc::Ring)));
    bool ok = mapped && ipc::compatibleRing(*ring) && ring->processCreated == created &&
        countersZero(ring->controlCounters) && countersZero(ring->pumpCounters) &&
        static_cast<uint32_t>(ipc::monitorState(*ring) >> 32) == kGeneration;
    if (ok) {
        WA_LOG(wa::log::Level::Info, "HookTransportProbe", "write/start", "pid=" + std::to_string(pid), "writers=4");
        std::atomic<bool> start{false};
        std::array<std::thread, kWriters> writers;
        for (uint32_t writer = 0; writer < kWriters; ++writer) {
            writers[writer] = std::thread([&, writer] {
                while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
                for (uint32_t row = 0; row < kWrites; ++row) {
                    ipc::writeCall(*ring, produce(pid, created, writer, row, false));
                    ipc::writeCall(*ring, produce(pid, created, writer, row, true));
                    if ((row & 255u) == 0) std::this_thread::yield();
                }
            });
        }
        start.store(true, std::memory_order_release);
        for (auto& writer : writers) writer.join();
        ok = ipc::atomicRead64(ring->controlCounters.reserved) == kAttempted &&
             ipc::atomicRead64(ring->pumpCounters.reserved) == kAttempted && ring->pumpXruns == 0;
        WA_LOG(wa::log::Level::Info, "HookTransportProbe", "write/end", "pid=" + std::to_string(pid), ok ? "complete" : "failed");
    }
    if (ring) ok = observed("UnmapViewOfFile/child", UnmapViewOfFile(ring) != FALSE, "child ring") && ok;
    closeHandle(mapping);
    return ok ? 0 : 12;
}

struct Lane {
    wa::HookTransportReader reader;
    wa::HookTransportLaneStatus status;
    std::vector<uint8_t> ids = std::vector<uint8_t>(kAttempted);
    std::vector<uint8_t> sequences = std::vector<uint8_t>(kAttempted);
    std::array<uint32_t, kWriters> perWriter{};
    uint64_t received = 0, invalid = 0, duplicates = 0, activeReceived = 0;
};

struct Target {
    PROCESS_INFORMATION process{};
    HANDLE mapping = nullptr;
    ipc::Ring* ring = nullptr;
    uint64_t created = 0;
    DWORD exitCode = STILL_ACTIVE;
    bool exited = false, initialized = false, initialCountersZero = false;
    uint64_t polls = 0;
    std::array<Lane, 2> lanes;
    ~Target() {
        if (process.hProcess && !exited) {
            observed("TerminateProcess/owned child", TerminateProcess(process.hProcess, 90) != FALSE,
                     "pid=" + std::to_string(process.dwProcessId), wa::log::Level::Warn);
            const DWORD wait = waitForChild(process.hProcess, 5000);
            WA_LOG(wait == WAIT_OBJECT_0 ? wa::log::Level::Info : wa::log::Level::Warn,
                   "HookTransportProbe", "child/cleanup", "owned child", "wait=" + std::to_string(wait));
        }
        if (ring) observed("UnmapViewOfFile/parent", UnmapViewOfFile(ring) != FALSE, "parent ring");
        closeHandle(mapping);
        closeHandle(process.hThread);
        closeHandle(process.hProcess);
    }
};

bool launch(Target& target, const wchar_t* self) {
    std::wstring command = L"\"" + std::wstring(self) + L"\" --child";
    STARTUPINFOW startup{sizeof(startup)};
    if (!observed("CreateProcessW", CreateProcessW(self, command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &target.process) != FALSE,
            "self --child CREATE_SUSPENDED | CREATE_NO_WINDOW", wa::log::Level::Info)) return false;
    if (!creationTime(target.process.hProcess, target.created)) return false;
    target.mapping = ipc::createHookMapping(target.process.dwProcessId);
    if (!observed("createHookMapping", target.mapping != nullptr,
                  "pid=" + std::to_string(target.process.dwProcessId))) return false;
    target.ring = static_cast<ipc::Ring*>(MapViewOfFile(target.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ipc::Ring)));
    if (!observed("MapViewOfFile/parent", target.ring != nullptr, "bytes=" + std::to_string(sizeof(ipc::Ring)))) return false;
    ipc::initializeRing(*target.ring);
    target.ring->processCreated = target.created;
    ipc::publishMonitorState(*target.ring, UINT32_MAX, kGeneration);
    target.ring->captureEnabled = 1;
    target.initialCountersZero = countersZero(target.ring->controlCounters) && countersZero(target.ring->pumpCounters);
    for (size_t lane = 0; lane < target.lanes.size(); ++lane)
        target.lanes[lane].reader.begin(*target.ring, lane != 0, kGeneration);
    target.initialized = true;
    return target.initialCountersZero;
}

void poll(Target& target, bool finish) {
    for (size_t index = 0; index < target.lanes.size(); ++index) {
        auto& lane = target.lanes[index];
        lane.reader.poll(*target.ring, [&](const ipc::CallPod& pod, uint64_t sequence) {
            ++lane.received;
            if (!target.exited) ++lane.activeReceived;
            uint32_t writer = 0;
            if (!validate(pod, target.process.dwProcessId, target.created, index != 0, writer) ||
                sequence >= kAttempted) { ++lane.invalid; return; }
            const uint32_t ordinal = (pod.streamId - 1) / 2;
            if (lane.ids[ordinal]++ || lane.sequences[static_cast<size_t>(sequence)]++) ++lane.duplicates;
            ++lane.perWriter[writer];
        }, finish);
    }
    ++target.polls;
}

bool complete(Target& target) {
    if (!target.initialized || !target.initialCountersZero || !target.exited || target.exitCode != 0 ||
        target.ring->processCreated != target.created || !ipc::compatibleRing(*target.ring) ||
        target.ring->pumpXruns != 0) return false;
    bool ok = true;
    for (size_t index = 0; index < target.lanes.size(); ++index) {
        auto& lane = target.lanes[index];
        lane.status = lane.reader.snapshot(*target.ring);
        const auto& counters = index ? target.ring->pumpCounters : target.ring->controlCounters;
        ok = ipc::atomicRead64(counters.reserved) == kAttempted &&
            lane.received + lane.status.overwritten + lane.status.writeRejected == kAttempted &&
            !lane.status.pending && !lane.status.unknown && !lane.invalid && !lane.duplicates &&
            lane.activeReceived != 0 && ok;
        for (const auto count : lane.perWriter) ok = count != 0 && ok;
    }
    return ok;
}

void laneJson(std::ostream& out, const Lane& lane, const ipc::LaneCounters* counters) {
    out << "{\"attempted\":" << kAttempted << ",\"reserved\":" << (counters ? ipc::atomicRead64(counters->reserved) : 0)
        << ",\"received\":" << lane.received << ",\"overwritten\":" << lane.status.overwritten
        << ",\"rejected\":" << lane.status.writeRejected << ",\"pending\":" << lane.status.pending
        << ",\"unknown\":" << lane.status.unknown << ",\"invalid\":" << lane.invalid
        << ",\"duplicates\":" << lane.duplicates << ",\"received_before_exit_observed\":" << lane.activeReceived
        << ",\"per_writer_received\":[";
    for (size_t i = 0; i < lane.perWriter.size(); ++i) out << (i ? "," : "") << lane.perWriter[i];
    out << "]}";
}

int parent(const std::filesystem::path& output) {
    if (!output.is_absolute()) return 20;
    wchar_t self[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)));
    if (!observed("GetModuleFileNameW", length != 0 && length < std::size(self), "self capacity=32768")) return 21;
    std::array<Target, 2> targets;
    bool ok = launch(targets[0], self) && launch(targets[1], self);
    if (ok) for (auto& target : targets) {
        const DWORD previous = ResumeThread(target.process.hThread);
        ok = observed("ResumeThread", previous != static_cast<DWORD>(-1),
                      "pid=" + std::to_string(target.process.dwProcessId) + " previous=" + std::to_string(previous)) &&
             previous == 1 && ok;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    size_t turn = 0;
    while (ok && !(targets[0].exited && targets[1].exited)) {
        // One bounded round per PID, alternating which PID runs first.
        for (size_t i = 0; i < targets.size(); ++i) {
            auto& target = targets[(turn + i) % targets.size()];
            poll(target, false);
            if (target.exited) continue;
            const DWORD wait = waitForChild(target.process.hProcess, 0);
            if (wait == WAIT_OBJECT_0) {
                target.exited = true;
                ok = observed("GetExitCodeProcess", GetExitCodeProcess(target.process.hProcess, &target.exitCode) != FALSE,
                              "pid=" + std::to_string(target.process.dwProcessId)) && ok;
                WA_LOG(wa::log::Level::Info, "HookTransportProbe", "child/exit",
                       "pid=" + std::to_string(target.process.dwProcessId), "code=" + std::to_string(target.exitCode));
            } else if (wait != WAIT_TIMEOUT) {
                ok = false;
            }
        }
        ++turn;
        if (std::chrono::steady_clock::now() >= deadline) { ok = false; break; }
        std::this_thread::yield();
    }
    for (auto& target : targets) if (target.initialized && target.exited) {
        poll(target, true);
        ok = complete(target) && ok;
    }
    std::ofstream out(output, std::ios::trunc);
    if (!out) return 22;
    out << std::boolalpha << "{\"success\":" << ok << ",\"synthetic_only\":true,\"real_injection\":false,\"real_audio\":false,"
        << "\"gui_performance\":false,\"slow_writer_or_wrap_or_generation_transition\":false,"
        << "\"layout\":" << ipc::kRingLayout << ",\"call_pod_bytes\":" << sizeof(ipc::CallPod)
        << ",\"ring_bytes\":" << sizeof(ipc::Ring) << ",\"pointer_bits\":" << sizeof(void*) * 8
        << ",\"slot_publication_bytes\":" << sizeof(ipc::SlotPublication) << ",\"generation\":" << kGeneration
        << ",\"writers_per_process\":" << kWriters << ",\"writes_per_writer_per_lane\":" << kWrites
        << ",\"control_capacity\":" << ipc::kCap << ",\"pump_capacity\":" << ipc::kPumpCap
        << ",\"control_poll_budget\":" << wa::HookTransportReader::kControlBudget
        << ",\"pump_poll_budget\":" << ipc::kPumpCap << ",\"finish_rounds_per_pid\":1,\"processes\":[";
    for (size_t i = 0; i < targets.size(); ++i) {
        const auto& target = targets[i];
        out << (i ? "," : "") << "{\"pid\":" << target.process.dwProcessId << ",\"created_filetime\":" << target.created
            << ",\"exited\":" << target.exited << ",\"exit_code\":" << target.exitCode
            << ",\"initialized\":" << target.initialized << ",\"initial_source_counters_zero\":" << target.initialCountersZero
            << ",\"poll_rounds\":" << target.polls << ",\"audio_xruns\":" << (target.ring ? target.ring->pumpXruns : 0)
            << ",\"control\":";
        laneJson(out, target.lanes[0], target.ring ? &target.ring->controlCounters : nullptr);
        out << ",\"pump\":";
        laneJson(out, target.lanes[1], target.ring ? &target.ring->pumpCounters : nullptr);
        out << "}";
    }
    out << "]}\n";
    out.close();
    return ok && out ? 0 : 23;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    wa::log::init();
    wa::log::addStderrSink();
    int code = 24;
    if (argc == 2 && !std::wcscmp(argv[1], L"--child")) code = child();
    else if (argc == 3 && !std::wcscmp(argv[1], L"--output")) code = parent(argv[2]);
    wa::log::shutdown();
    return code;
}
