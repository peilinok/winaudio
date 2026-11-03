// Release-only formal mode of the same controlled render + endpoint loopback
// target. All loop-body samples survive until the child has actually exited.
namespace {
int audioPerformance(const std::filesystem::path& directory, unsigned warmup, unsigned seconds) {
    if (!directory.is_absolute() || !seconds || seconds > 120) return 60;
    std::filesystem::create_directories(directory);
    std::ofstream raw(directory / "audio-samples.csv"), summary(directory / "audio-summary.csv"),
        boundaries(directory / "audio-boundaries.csv");
    if (!raw || !summary || !boundaries) return 61;
    raw << "repeat,state,target,pid,sample,start_qpc,ticks\n";
    summary << "repeat,state,target,pid,created_filetime,callbacks,original_calls,failures,discontinuities,captured_frames,samples,overflow,mean_us,p50_us,p95_us,p99_us,max_us,min_us,exit\n";
    boundaries << "repeat,state,warmup_start,formal_start,formal_end,qpc_frequency\n";
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    const auto tick = [&] { QueryPerformanceCounter(&now); return now.QuadPart; };
    const char* names[] = {"unattached", "installed-mask0", "default", "high-frequency"};
    bool ok = true;
    for (unsigned repeat = 1; repeat <= 3; ++repeat) {
        std::array<Target, 2> targets;
        std::array<wa::ProcessInstance, 2> identities{};
        for (size_t i = 0; i < targets.size(); ++i) {
            if (!launch(targets[i], i, 200, -1.f, true) || !wa::queryProcessInstance(targets[i].pid, identities[i])) return 62;
            const auto& s = *targets[i].shared;
            std::printf("audio_target repeat=%u target=%zu pid=%lu created=%llu endpoint=%ls rate=%lu channels=%u bits=%u valid_bits=%u channel_mask=%lu tag=%u buffer_frames=%u sleep_ms=5 silent_render=1\n",
                repeat, i, targets[i].pid, identities[i].created, s.endpoint, s.sampleRate, s.channels,
                s.bits, s.validBits, s.channelMask, s.formatTag, s.bufferFrames);
            InterlockedExchange(&targets[i].shared->phase, static_cast<LONG>(kPhases));
        }
        const auto waitUntil = [&](int64_t deadline) {
            while (tick() < deadline) {
                for (auto& target : targets) {
                    const DWORD waited = WaitForSingleObject(target.process, 0);
                    if (waited != WAIT_TIMEOUT) {
                        observe("WaitForSingleObject/performance", waited == WAIT_FAILED ?
                            HRESULT_FROM_WIN32(GetLastError()) : E_UNEXPECTED);
                        return false;
                    }
                }
                Sleep(10);
            }
            return true;
        };
        for (unsigned phase = 0; phase < 4; ++phase) {
            if (phase == 1) for (auto& target : targets) if (!attach(target)) return 63;
            if (phase) for (auto& target : targets) {
                const uint32_t mask = phase == 1 ? 0 : phase == 2 ? wa::kDefaultMonitorMethods : wa::kAllMonitorMethods;
                if (!target.attach.publishMethods(target.attach.instance(), mask)) return 64;
                target.attach.setCaptureEnabled(true);
                std::printf("audio_state repeat=%u state=%s pid=%lu mask=%u\n", repeat, names[phase], target.pid, mask);
            }
            const int64_t warmStart = tick();
            std::printf("audio_warmup repeat=%u state=%s qpc=%lld seconds=%u\n", repeat, names[phase], warmStart, warmup);
            std::fflush(stdout);
            if (!waitUntil(warmStart + static_cast<int64_t>(warmup) * frequency.QuadPart)) return 65;
            // Publish a future start before enabling the phase. Ownership is by
            // loop start QPC; a loop crossing the end remains in the raw tail.
            const int64_t formalStart = tick() + frequency.QuadPart / 50;
            const int64_t formalEnd = formalStart + static_cast<int64_t>(seconds) * frequency.QuadPart;
            for (auto& target : targets) {
                InterlockedExchange64(&target.shared->formalStart, formalStart);
                InterlockedExchange64(&target.shared->formalEnd, formalEnd);
                InterlockedExchange(&target.shared->phase, static_cast<LONG>(phase));
            }
            if (!waitUntil(formalEnd)) return 66;
            for (auto& target : targets) InterlockedExchange(&target.shared->phase, static_cast<LONG>(kPhases));
            boundaries << repeat << ',' << names[phase] << ',' << warmStart << ',' << formalStart << ',' << formalEnd << ',' << frequency.QuadPart << '\n';
            std::printf("audio_formal_complete repeat=%u state=%s start=%lld end=%lld\n", repeat, names[phase], formalStart, formalEnd);
            std::fflush(stdout);
        }
        for (auto& target : targets) InterlockedExchange(&target.shared->stop, 1);
        for (size_t i = 0; i < targets.size(); ++i) {
            auto& target = targets[i];
            const DWORD waited = WaitForSingleObject(target.process, 10000);
            observe("WaitForSingleObject/performance-exit", waited == WAIT_OBJECT_0 ? S_OK :
                HRESULT_FROM_WIN32(waited == WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT));
            if (waited != WAIT_OBJECT_0) return 67;
            DWORD exit = STILL_ACTIVE;
            if (!observeWin32("GetExitCodeProcess/performance", GetExitCodeProcess(target.process, &exit) != FALSE)) return 68;
            if (exit) ok = false;
            for (unsigned phase = 0; phase < 4; ++phase) {
                const auto& s = target.shared->stats[phase];
                std::vector<int64_t> ticks(s.ticks, s.ticks + s.count);
                for (size_t n = 0; n < ticks.size(); ++n)
                    raw << repeat << ',' << names[phase] << ',' << i << ',' << target.pid << ',' << n << ',' << s.starts[n] << ',' << ticks[n] << '\n';
                const double mean = ticks.empty() ? 0 : std::accumulate(ticks.begin(), ticks.end(), 0.0) / ticks.size();
                std::sort(ticks.begin(), ticks.end());
                const double scale = 1e6 / frequency.QuadPart;
                const auto rank = [&](size_t p) { return ticks.empty() ? 0.0 : ticks[(ticks.size() * p + 99) / 100 - 1] * scale; };
                summary << repeat << ',' << names[phase] << ',' << i << ',' << target.pid << ',' << identities[i].created << ','
                    << s.callbacks << ',' << s.originalCalls << ',' << s.failures << ',' << s.discontinuities << ',' << s.capturedFrames << ','
                    << s.count << ',' << s.sampleOverflow << ',' << mean * scale << ',' << rank(50) << ',' << rank(95) << ',' << rank(99) << ','
                    << (ticks.empty() ? 0 : ticks.back() * scale) << ',' << (ticks.empty() ? 0 : ticks.front() * scale) << ',' << exit << '\n';
                if (!s.callbacks || !s.capturedFrames || s.failures || s.sampleOverflow || s.count != s.callbacks) ok = false;
            }
            // Warmup remains separately inspectable; it is never pooled into formal percentiles.
            const auto& warm = target.shared->stats[kPhases];
            std::printf("audio_warmup_total repeat=%u pid=%lu callbacks=%llu failures=%llu discontinuities=%llu overflow=%u exit=%lu\n",
                repeat, target.pid, warm.callbacks, warm.failures, warm.discontinuities, warm.sampleOverflow, exit);
        }
    }
    raw.close(); summary.close(); boundaries.close();
    ok = raw && summary && boundaries && ok;
    std::printf("result=%s repeats=3 states=4 warmup=%u formal=%u qpc_frequency=%lld timing=audio-loop-body-excludes-periodic-controls-and-Sleep\n",
        ok ? "PASS" : "FAIL", warmup, seconds, frequency.QuadPart);
    return ok ? 0 : 69;
}
}
