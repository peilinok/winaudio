#include "HookBufferHarness.h"
#include <algorithm>
#include <vector>
#include <filesystem>
#include <fstream>
#include <numeric>

int main(int argc, char** argv) {
    const bool pipeline = argc == 3 && std::string(argv[1]) == "--pipeline";
    if (argc != 1 && !pipeline) return 2;
    std::ofstream raw;
    if (pipeline) {
        raw.open(std::filesystem::path(argv[2]), std::ios::trunc);
        if (!raw) return 3;
        raw << "repeat,entry,state,sample,ticks\n";
    }
    LARGE_INTEGER frequency{}, a{}, b{};
    QueryPerformanceFrequency(&frequency);
    constexpr int samples = 20000, batch = 300000;
    const char* states[] = {"direct", "no-ring", "quiet", "capture-off", "pump-off", "enabled"};
    const char* pipelineStates[] = {"direct", "installed-mask0", "default", "high-frequency"};
    std::printf("# Production hooks; observer compiled out; QPC frequency=%lld; batch=%d; samples=%d\n",
                frequency.QuadPart, batch, samples);
    std::printf("# layout=%u sizeof_call=%zu sizeof_ring=%zu capture_offset=%zu pump_offset=%zu\n",
                wa::hook_ipc::kRingLayout, sizeof(CallPod), sizeof(Ring),
                offsetof(Ring, captureEnabled), offsetof(Ring, pumpEnabled));
    QueryPerformanceCounter(&a);
    for (int i = 0; i < samples; ++i) QueryPerformanceCounter(&b);
    QueryPerformanceCounter(&b);
    std::printf("# QPC loop ns/call=%.3f; volatile original-call counter prevents elimination\n",
                1e9 * static_cast<double>(b.QuadPart - a.QuadPart) / frequency.QuadPart / samples);
    std::printf("repeat,entry,state,batch_ns,p50_ns,p95_ns,p99_ns,calls,mean_ns,max_ns,minimum_ns\n");
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (int entry = 0; entry < 4; ++entry) {
            for (int state = 0; state < (pipeline ? 4 : 6); ++state) {
                hook_harness::reset(pipeline ? 5 : state);
                if (pipeline) wa::hook_ipc::publishMonitorState(hook_harness::ring,
                    state == 1 ? 0 : state == 2 ? wa::kDefaultMonitorMethods : wa::kAllMonitorMethods, 0);
                if (pipeline) std::printf("# state_metadata repeat=%d entry=%d state=%s mask=%u generation=0 capture=1 mapped=1 direct_original=%d\n",
                    repeat + 1, entry, pipelineStates[state], state == 1 ? 0 :
                    state == 2 ? wa::kDefaultMonitorMethods : wa::kAllMonitorMethods, state == 0);
                for (int i = 0; i < 10000; ++i) hook_harness::invoke(entry, state == 0);
                QueryPerformanceCounter(&a);
                for (int i = 0; i < batch; ++i) hook_harness::invoke(entry, state == 0);
                QueryPerformanceCounter(&b);
                const double average = 1e9 * static_cast<double>(b.QuadPart - a.QuadPart) /
                                       frequency.QuadPart / batch;
                std::vector<LONGLONG> ticks(samples);
                for (auto& t : ticks) {
                    QueryPerformanceCounter(&a);
                    hook_harness::invoke(entry, state == 0);
                    QueryPerformanceCounter(&b);
                    t = b.QuadPart - a.QuadPart;
                }
                const char* label = pipeline ? pipelineStates[state] : states[state];
                if (pipeline) for (size_t i = 0; i < ticks.size(); ++i)
                    raw << repeat + 1 << ',' << entry << ',' << label << ',' << i << ',' << ticks[i] << '\n';
                const double mean = std::accumulate(ticks.begin(), ticks.end(), 0.0) / samples;
                std::sort(ticks.begin(), ticks.end());
                const double scale = 1e9 / frequency.QuadPart;
                std::printf("%d,%d,%s,%.3f,%.3f,%.3f,%.3f,%u,%.3f,%.3f,%.3f\n", repeat + 1, entry, label,
                            average, ticks[samples / 2 - 1] * scale, ticks[samples * 95 / 100 - 1] * scale,
                            ticks[samples * 99 / 100 - 1] * scale, hook_harness::calls,
                            mean * scale, ticks.back() * scale, ticks.front() * scale);
            }
        }
    }
    if (pipeline) { raw.close(); if (!raw) return 4; }
    std::printf("result=PASS samples=%d batch=%d warmup=10000 repeats=3 observer=compiled-out\n", samples, batch);
    return 0;
}
