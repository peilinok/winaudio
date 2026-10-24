#include "HookBufferHarness.h"
#include <algorithm>
#include <vector>

int main() {
    LARGE_INTEGER frequency{}, a{}, b{};
    QueryPerformanceFrequency(&frequency);
    constexpr int samples = 20000, batch = 300000;
    const char* states[] = {"direct", "no-ring", "quiet", "capture-off", "pump-off", "enabled"};
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
    std::printf("repeat,entry,state,batch_ns,p50_ns,p95_ns,p99_ns,calls\n");
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (int entry = 0; entry < 4; ++entry) {
            for (int state = 0; state < 6; ++state) {
                hook_harness::reset(state);
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
                std::sort(ticks.begin(), ticks.end());
                const double scale = 1e9 / frequency.QuadPart;
                std::printf("%d,%d,%s,%.3f,%.3f,%.3f,%.3f,%u\n", repeat + 1, entry, states[state],
                            average, ticks[samples / 2 - 1] * scale, ticks[samples * 95 / 100 - 1] * scale,
                            ticks[samples * 99 / 100 - 1] * scale, hook_harness::calls);
            }
        }
    }
}
