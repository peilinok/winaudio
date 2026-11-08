#include "ChartDataPipeline.h"
#include "ScopeBuffer.h"
#include "WaveEnvelope.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
volatile double observed = 0;
FILE* raw = nullptr;
int repetition = 0;

template<class F> void measure(const std::string& name, int count, F fn) {
    for (int i = 0; i < (std::min)(count, 20); ++i) fn();
    std::vector<double> times;
    times.reserve(count);
    for (int i = 0; i < count; ++i) {
        const auto before = Clock::now();
        fn();
        times.push_back(std::chrono::duration<double, std::micro>(Clock::now() - before).count());
    }
    for (size_t i = 0; i < times.size(); ++i)
        std::fprintf(raw, "%d,%s,%zu,%.3f\n", repetition, name.c_str(), i, times[i]);
    const double sum = std::accumulate(times.begin(), times.end(), 0.0);
    std::sort(times.begin(), times.end());
    const auto p = [&](double q) { return times[static_cast<size_t>(std::ceil(q * count)) - 1]; };
    std::printf("%d,%s,%d,%.3f,%.3f,%.3f,%.3f,%.3f\n", repetition, name.c_str(), count,
                sum / count, p(.5), p(.95), p(.99), times.back());
    std::fflush(stdout);
}

void require(bool ok) { if (!ok) throw std::runtime_error("测量路径未完成预期工作"); }

struct Reader : wa::ScopeReader {
    wa::ScopeBuffer buffer;
    explicit Reader(uint16_t ch) : buffer(1048576, ch) {}
    uint64_t written() const override { return buffer.totalWritten(); }
    uint16_t channels() const override { return buffer.channels(); }
    bool snapshotLatest(size_t n, float* out, uint64_t& end) const override { return buffer.snapshotLatest(n, out, end); }
    bool snapshotEndingAt(uint64_t end, size_t n, float* out) const override { return buffer.snapshotEndingAt(end, n, out); }
    bool snapshotChannelEndingAt(uint16_t ch, uint64_t end, size_t n, float* out) const override {
        return buffer.snapshotChannelEndingAt(ch, end, n, out);
    }
};

void charts() {
    for (uint16_t ch : {uint16_t(1), uint16_t(2), uint16_t(8)}) {
        Reader reader(ch);
        std::vector<float> initial(491520 * ch, .125f), block(800 * ch, .125f);
        reader.buffer.pushInterleaved(initial.data(), 491520);
        wa::WaveRefreshState state;
        wa::WaveSnapshotScratch scratch;
        std::vector<std::vector<float>> waves(ch, std::vector<float>(491520)), windows(ch, std::vector<float>(2048));
        std::vector<std::unique_ptr<wa::Spectrogram>> owned;
        std::vector<wa::Spectrogram*> specs;
        for (uint16_t i = 0; i < ch; ++i) {
            owned.emplace_back(new wa::Spectrogram(128, 480, 20, 24000, 48000)); specs.push_back(owned.back().get());
        }
        std::vector<std::complex<float>> work(2048);
        std::vector<float> mag;
        uint64_t cursor = 0;
        wa::ChartBuffers b;
        b.wave = waves[0].data(); b.channelWaves = &waves; b.channelCount = ch;
        b.waveN = 491520; b.waveSr = 48000; b.waveState = &state; b.waveScratch = &scratch;
        b.specWin = &windows[0]; b.channelSpecWindows = &windows;
        b.work = &work; b.mag = &mag; b.spectrogram = specs[0];
        b.channelSpecs = specs.data(); b.channelSpecCount = ch; b.nextEnd = &cursor; b.specSr = 48000;
        wa::ChartRefreshParams p; p.reader = &reader; p.streamActive = true;
        require(wa::refreshCharts(p, b).haveWave);
        const auto suffix = "-" + std::to_string(ch) + "ch-800frames";
        measure("chart-push-refresh" + suffix, 1000, [&] {
            reader.buffer.pushInterleaved(block.data(), 800); require(wa::refreshCharts(p, b).waveChanged);
            observed = state.view(waves[0].data())[b.waveN - 1];
        });
        b.specSr = 0;
        measure("chart-wave-only" + suffix, 1000, [&] {
            reader.buffer.pushInterleaved(block.data(), 800); require(wa::refreshCharts(p, b).waveChanged);
            observed = state.view(waves[0].data())[b.waveN - 1];
        });
        p.frozen = true;
        measure("chart-frozen" + suffix, 2000, [&] { require(wa::refreshCharts(p, b).haveWave); });
    }
}


void envelopes() {
    constexpr int n = 491520;
    std::vector<float> samples(n), x(1000), lo(1000), hi(1000);
    for (int i = 0; i < n; ++i) samples[static_cast<size_t>(i)] = .5f * std::sin(static_cast<float>(i) * .02f);
    wa::WaveHistoryView view{samples.data(), n, 0, n};
    wa::MinMaxPyramid pyramid;
    uint64_t epoch = 0;
    measure("wave-envelope-moving-491520", 1500, [&] {
        for (int i = 0; i < 800; ++i) samples[static_cast<size_t>((view.first + i) % n)] = .125f;
        view.first = (view.first + 800) % n;
        view.updatedSamples += 800;
        require(wa::fillWaveEnvelope(view, 0, n - 1, 1000, 1.0 / 48000,
            &pyramid, ++epoch, x.data(), lo.data(), hi.data()));
        observed = hi[0];
    });
    measure("wave-envelope-frozen-491520", 1500, [&] {
        require(wa::fillWaveEnvelope(view, 0, n - 1, 1000, 1.0 / 48000,
            &pyramid, epoch, x.data(), lo.data(), hi.data()));
        observed = hi[0];
    });
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    raw = std::fopen(argv[1], "wb");
    if (!raw) return 3;
    std::fprintf(raw, "repeat,case,sample,microseconds\n");
    std::printf("repeat,case,n,mean_us,p50_us,p95_us,p99_us,max_us\n");
    int code = 0;
    try {
        for (repetition = 1; repetition <= 3; ++repetition) { charts(); envelopes(); }
    } catch (const std::exception& ex) { std::fprintf(stderr, "%s\n", ex.what()); code = 4; }
    std::fclose(raw);
    return code;
}
