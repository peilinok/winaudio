#include "AudioFormat.h"
#include "WavFile.h"
#include "WavSink.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kCalls = 1000;
constexpr size_t kFrames = 480;
constexpr auto kPeriod = std::chrono::milliseconds(10);

struct Measurement {
    std::vector<int64_t> nanos;
    size_t accepted = 0;
    double seconds = 0;
};

Measurement measure(wa::WavSink& sink, const std::vector<float>& pcm) {
    Measurement result;
    result.nanos.reserve(kCalls);
    const auto started = Clock::now();
    auto next = started;
    for (size_t i = 0; i < kCalls; ++i) {
        std::this_thread::sleep_until(next);
        const auto before = Clock::now();
        const size_t accepted = sink.push(pcm.data(), pcm.size() * sizeof(float));
        const auto after = Clock::now();
        result.accepted += accepted;
        result.nanos.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count());
        next += kPeriod;
    }
    result.seconds = std::chrono::duration<double>(Clock::now() - started).count();
    return result;
}

void print(const char* phase, Measurement result) {
    std::sort(result.nanos.begin(), result.nanos.end());
    const auto percentile = [&](size_t percent) {
        return result.nanos[(result.nanos.size() * percent + 99) / 100 - 1];
    };
    const double pushSeconds = static_cast<double>(std::accumulate(
        result.nanos.begin(), result.nanos.end(), int64_t{0})) / 1e9;
    const size_t offered = kCalls * kFrames * 2 * sizeof(float);
    std::printf("%s,%zu,%zu,%zu,%.6f,%.3f,%.3f,%.3f,%lld,%lld,%lld\n", phase,
        result.nanos.size(), offered, result.accepted, result.seconds,
        static_cast<double>(result.nanos.size()) / result.seconds,
        static_cast<double>(result.accepted) / result.seconds,
        static_cast<double>(offered) / pushSeconds,
        static_cast<long long>(percentile(50)), static_cast<long long>(percentile(95)),
        static_cast<long long>(percentile(99)));
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: WavSinkProbe <output.wav>\n");
        return 2;
    }
    const wa::AudioFormat format{48000, 2, 32, true};
    std::vector<float> pcm(kFrames * 2);
    for (size_t i = 0; i < pcm.size(); ++i)
        pcm[i] = static_cast<float>(static_cast<int>(i % 31) - 15) / 32.f;
    wa::WavSink sink;
    const auto started = sink.startExact(argv[1], format);
    if (!started) {
        std::fprintf(stderr, "start: %s\n", started.message.c_str());
        return 3;
    }
    auto normal = measure(sink, pcm);
    const auto beforeStop = Clock::now();
    const auto stopped = sink.stop();
    const auto stopNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - beforeStop).count();
    if (!stopped || normal.accepted != kCalls * pcm.size() * sizeof(float)) {
        std::fprintf(stderr, "normal path failed: %s, accepted=%zu\n", stopped.message.c_str(), normal.accepted);
        return 4;
    }
    wa::WavReader reader;
    if (!reader.open(argv[1]) || !(reader.format() == format)) return 5;
    std::vector<float> readback(pcm.size());
    for (size_t i = 0; i < kCalls; ++i) {
        if (reader.read(readback.data(), readback.size() * sizeof(float)) != readback.size() * sizeof(float)
            || readback != pcm) return 6;
    }
    if (!reader.eof() || !reader.close()) return 7;
    auto closed = measure(sink, pcm);
    if (closed.accepted != 0) return 8;
    std::printf("# format=48000/32f/2 frames=480 period_us=10000 calls=1000 writer=real-file stdio=262144 batch=262144 poll_ms=5\n");
    std::printf("# clock=steady_clock timing_includes_two_clock_reads stop_ns=%lld readback=verified\n", static_cast<long long>(stopNanos));
    std::printf("phase,calls,offered_bytes,accepted_bytes,wall_s,calls_per_s,accepted_bytes_per_s,offered_bytes_per_timed_push_s,p50_ns,p95_ns,p99_ns\n");
    print("normal", std::move(normal));
    print("closed", std::move(closed));
    return 0;
}
