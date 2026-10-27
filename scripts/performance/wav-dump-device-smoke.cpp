#include "CaptureTrackList.h"
#include "AudioFormatStr.h"
#include "ComUtil.h"
#include "Log.h"
#include "RingBuffer.h"
#include "WasapiStream.h"
#include "WavFile.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

bool require(bool ok, const char* operation) {
    if (!ok) std::fprintf(stderr, "FAILED: %s\n", operation);
    return ok;
}

template <typename Predicate>
bool waitFor(Predicate predicate) {
    const auto until = Clock::now() + std::chrono::seconds(5);
    do {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (Clock::now() < until);
    return predicate();
}

struct Producer {
    wa::RingBuffer& ring;
    std::atomic<bool> running{true};
    std::thread thread;
    Producer(wa::RingBuffer& buffer, const wa::AudioFormat& format) : ring(buffer) {
        const size_t frames = format.sampleRate / 100;
        thread = std::thread([this, frames, format] {
            std::vector<float> block(frames * format.channels);
            for (size_t i = 0; i < block.size(); ++i)
                block[i] = (i % 4 < 2) ? .001f : -.001f;
            auto next = Clock::now();
            while (running.load()) {
                ring.write(block.data(), block.size() * sizeof(float));
                next += std::chrono::milliseconds(10);
                std::this_thread::sleep_until(next);
            }
        });
    }
    ~Producer() { running.store(false); if (thread.joinable()) thread.join(); }
};

bool verify(const wa::CaptureTrackStatus& status, unsigned round) {
    wa::WavReader reader;
    if (!require(static_cast<bool>(reader.open(status.dumpPath)), "read completed WAV")) return false;
    if (!require(reader.format() == status.actualFormat, "WAV actual format")) return false;
    if (status.actualFormat.channels > 2 &&
        !require(reader.format().channelMask == wa::toWaveFormatExtensible(status.actualFormat).dwChannelMask,
                 "WAV channel mask")) return false;
    std::vector<uint8_t> block(65536);
    size_t bytes = 0;
    size_t nonzero = 0;
    for (;;) {
        const size_t got = reader.read(block.data(), block.size());
        if (!got) break;
        bytes += got;
        nonzero += static_cast<size_t>(std::count_if(block.begin(), block.begin() + got,
            [](uint8_t value) { return value != 0; }));
    }
    if (!require(bytes > 0 && nonzero > 0 && bytes % status.actualFormat.blockAlign() == 0 && reader.eof(),
                 "nonempty complete PCM frames")) return false;
    std::printf("round=%u source=%s rate=%u channels=%u bits=%u float=%d bytes=%zu nonzero_bytes=%zu file=%s\n",
        round, status.source.kind == wa::CaptureSourceKind::SystemLoopback ? "system" : "application",
        status.actualFormat.sampleRate, status.actualFormat.channels, status.actualFormat.bitsPerSample,
        status.actualFormat.isFloat ? 1 : 0, bytes, nonzero, wa::narrowAscii(status.dumpPath).c_str());
    return require(static_cast<bool>(reader.close()), "reader close");
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool sourceOnly = argc == 4 && std::wstring(argv[3]) == L"--source-only";
    if (argc != 3 && !sourceOnly) {
        std::fprintf(stderr, "usage: WavDumpDeviceSmoke <virtual-render-endpoint-id> <existing-output-folder> [--source-only]\n");
        return 2;
    }
    wa::log::init();
    struct LogShutdown { ~LogShutdown() { wa::log::shutdown(); } } logging;
    wa::log::addFileSink(wa::narrowAscii(std::wstring(argv[2]) + L"\\runtime.txt"));
    wa::ComInitGuard com;
    if (!require(com.ok(), "COM initialize")) return 3;
    wa::RingBuffer ring(1u << 20);
    wa::WasapiRenderStream render(wa::WasapiMode::Shared, nullptr);
    if (!require(static_cast<bool>(render.open(argv[1], {}, &ring, {})), "open virtual render")
        || !require(static_cast<bool>(render.start()), "start virtual render")) return 4;
    const auto format = render.stats().actualFormat;
    if (!require(format.isFloat && format.bitsPerSample == 32, "Float32 render mix")) return 5;
    Producer producer(ring, format);
    if (sourceOnly) {
        std::printf("source_pid=%lu duration_s=180\n", GetCurrentProcessId());
        std::fflush(stdout);
        for (unsigned i = 0; i < 180; ++i) {
            if (render.inError()) return 19;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return 0;
    }
    wa::CaptureTrackList tracks;
    wa::TrackId system = 0, application = 0;
    wa::CaptureTrackCreate recipe;
    recipe.source.kind = wa::CaptureSourceKind::SystemLoopback;
    recipe.source.deviceId = argv[1];
    recipe.loopbackOptions.silentRender = false;
    if (!require(static_cast<bool>(tracks.create(recipe, &system)), "create System Loopback")) return 6;
    recipe.source = {};
    recipe.source.kind = wa::CaptureSourceKind::ApplicationLoopback;
    recipe.source.processId = GetCurrentProcessId();
    if (!require(static_cast<bool>(tracks.create(recipe, &application)), "create Application Loopback")) return 7;
    if (!require(waitFor([&] { return tracks.written(system) > 0 && tracks.written(application) > 0; }),
                 "both real capture Scope taps advance")) return 8;
    for (unsigned round = 1; round <= 3; ++round) {
        const auto systemBefore = tracks.written(system);
        const auto appBefore = tracks.written(application);
        if (!require(static_cast<bool>(tracks.startDump(system, argv[2])), "start System Dump")
            || !require(static_cast<bool>(tracks.startDump(application, argv[2])), "start Application Dump")) return 9;
        if (!require(waitFor([&] {
            return tracks.written(system) >= systemBefore + 4800 && tracks.written(application) >= appBefore + 4800;
        }), "real PCM while dumping")) return 10;
        auto completed = tracks.poll();
        if (round == 3) {
            tracks.destroyAll();
            if (!require(tracks.poll().empty(), "destroy all")) return 11;
        } else {
            if (!require(static_cast<bool>(tracks.stopDump(system)), "stop System Dump")) return 12;
            const auto systemStopped = tracks.written(system);
            const auto appContinuing = tracks.written(application);
            if (!require(waitFor([&] {
                return tracks.written(system) > systemStopped && tracks.written(application) > appContinuing;
            }), "System tap and Application sibling continue after System Dump stops")) return 13;
            if (!require(static_cast<bool>(tracks.stopDump(application)), "stop Application Dump")) return 14;
            const auto appStopped = tracks.written(application);
            if (!require(waitFor([&] { return tracks.written(application) > appStopped; }),
                         "Application tap continues after Dump stops")) return 15;
            for (const auto& status : tracks.poll()) {
                if (!require(status.state == wa::StreamState::Running && !status.dumping && !status.dumpError,
                             "tracks remain Running without Dump failure")) return 16;
            }
            std::printf("round=%u continuing_scope_system=%llu continuing_scope_application=%llu\n", round,
                static_cast<unsigned long long>(tracks.written(system)),
                static_cast<unsigned long long>(tracks.written(application)));
        }
        if (!require(completed.size() == 2, "both completed file identities")) return 17;
        for (const auto& status : completed) if (!verify(status, round)) return 18;
    }
    if (!require(!render.inError(), "render remains healthy")) return 19;
    std::printf("PASS: three independent rounds for both real loopback sources; final active Dumps finalized by destroyAll\n");
    return 0;
}
