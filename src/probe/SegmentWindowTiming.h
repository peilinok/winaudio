// Timed mode shares the production Hook entries, transport, and Store with the
// deterministic probe above. It does not inject a DLL or claim real audio.
namespace {
int timedParent(const std::filesystem::path& directory, const std::wstring& mode,
                unsigned warmup, unsigned seconds, bool stall) {
    const bool gui = mode.find(L"gui-") == 0;
    const std::wstring load = gui ? mode.substr(4) : mode;
    if (!directory.is_absolute() || !seconds || (load != L"regular" && load != L"overload" && load != L"memory")) return 40;
    std::filesystem::create_directories(directory);
    wchar_t self[32768]{};
    if (!GetModuleFileNameW(nullptr, self, static_cast<DWORD>(std::size(self)))) return 41;
    LARGE_INTEGER frequency{}, now{};
    QueryPerformanceFrequency(&frequency);
    const auto tick = [&] { QueryPerformanceCounter(&now); return static_cast<uint64_t>(now.QuadPart); };
    const bool memory = load == L"memory";
    probeTimed = true;
    const unsigned rates[] = {load == L"overload" ? 22000u : 2100u, load == L"overload" ? 101u : 2100u};
    wa::CallLogStore store;
    store.setPumpVisible(true);
    std::array<ProbeTarget, 2> targets;
    bool ok = true;
    for (unsigned i = 0; i < targets.size(); ++i) {
        ok = probeLaunch(targets[i], i, self, store, rates[i], warmup, seconds, !memory) && ok;
        if (!ok || probeWait(targets[i].handles.ready, 10000) != WAIT_OBJECT_0) return 42;
        std::printf("ready target=%u pid=%u created=%llu synthetic_only=1 rate=%u controls=%u\n", i,
            targets[i].instance.pid, targets[i].instance.created, rates[i], memory ? 0 : 1);
    }
    std::fflush(stdout);
    if (gui) {
        const auto readyDeadline = tick() + static_cast<uint64_t>(frequency.QuadPart) * 120;
        while (!std::filesystem::exists(directory / "gui-ready.flag")) {
            if (std::filesystem::exists(directory / "owner-cancel.flag")) return 49;
            if (tick() >= readyDeadline) return 47;
            Sleep(10);
        }
    }
    const uint64_t start = tick() + static_cast<uint64_t>(frequency.QuadPart) / 2;
    if (gui) {
        std::ofstream origin(directory / "source-start.txt");
        origin << start << '\n';
        origin.close();
        if (!origin) return 48;
    }
    for (auto& target : targets) {
        target.handles.shared->start = start;
        if (!probeSet(target.handles.resume)) return 43;
    }
    std::ofstream samples(directory / "samples.csv"), windows(directory / "windows.jsonl");
    if (!samples || !windows) return 44;
    samples << "qpc,elapsed_s,pid,created_filetime,originals,controls,received,retained,store_controls,source_completed,source_failed,overwritten,rejected,pending,unknown,stalled,finished\n";
    uint64_t nextSample = start;
    std::array<std::vector<std::string>, 2> latest;
    std::array<std::array<uint32_t, ipc::kPumpCap>, 2> lastRow{};
    const auto consume = [&](unsigned i) {
        auto& target = targets[i];
        target.window.poll(*target.handles.ring);
        auto received = target.window.takeReceived();
        for (const auto& item : received) {
            ++target.received;
            if (item.sequence != target.received || !item.call.pump ||
                item.call.observationGeneration != kGeneration) ++target.invalid;
            uint32_t row = 0;
            if (i == 0) {
                unsigned long long hz = 0;
                if (SUCCEEDED(item.call.hresult) && sscanf_s(item.call.args.c_str(), "hz=%llu", &hz) == 1)
                    row = static_cast<uint32_t>(hz - 100000);
            } else sscanf_s(item.call.args.c_str(), "frames=%u", &row);
            if (row) {
                auto& previous = lastRow[i][row % ipc::kPumpCap];
                if (row <= previous || !probeValidate(item.call, i, row)) ++target.invalid;
                previous = row;
            } else if (item.call.hresult != E_ACCESSDENIED) ++target.invalid;
            auto expected = item.call;
            expected.pid = target.instance.pid;
            expected.processCreated = target.instance.created;
            expected.attachToken = i + 1;
            latest[i].push_back(wa::CallLogStore::toJsonLine(expected));
            if (latest[i].size() > kRecent) latest[i].erase(latest[i].begin());
        }
        store.appendPump(target.attach, received);
        target.controls.poll(*target.handles.ring, [&](const ipc::CallPod& pod, uint64_t) {
            ++target.controlReceived;
            if (std::string(pod.method) != "Start" || pod.hresult != S_OK) ++target.invalid;
            store.append(target.attach, wa::decodeHookedCallPod(pod));
        });
    };
    const uint64_t deadline = start + static_cast<uint64_t>(warmup + seconds + 20) * frequency.QuadPart;
    bool done = false;
    uint64_t stallStart = 0, stallEnd = 0, stalledOriginalsStart = 0, stalledOriginalsEnd = 0,
        otherReceivedStart = 0, otherReceivedEnd = 0;
    while (!done && tick() < deadline) {
        if (std::filesystem::exists(directory / "owner-cancel.flag")) return 49;
        const uint64_t current = static_cast<uint64_t>(now.QuadPart);
        const double elapsed = current >= start ? static_cast<double>(current - start) / frequency.QuadPart : -1;
        done = true;
        for (unsigned i = 0; i < targets.size(); ++i) {
            auto& target = targets[i];
            const bool withholding = stall && i == 0 && elapsed >= warmup + 1.0 && elapsed < warmup + 2.0;
            if (i == 0 && withholding && !stallStart) {
                stallStart = current;
                stalledOriginalsStart = static_cast<uint64_t>(ReadAcquire(&target.handles.shared->originals));
                otherReceivedStart = targets[1].received;
            } else if (i == 0 && !withholding && stallStart && !stallEnd) {
                stallEnd = current;
                stalledOriginalsEnd = static_cast<uint64_t>(ReadAcquire(&target.handles.shared->originals));
                otherReceivedEnd = targets[1].received;
            }
            if (!gui && !withholding) consume(i);
            done = ReadAcquire(&target.handles.shared->finished) && done;
            if (current >= nextSample) {
                const auto stats = ipc::readMethodStatistics(*target.handles.ring, kGeneration);
                const auto loss = target.window.transportStatus(*target.handles.ring);
                const auto& method = stats.methods[static_cast<unsigned>(probeMethod(i))];
                samples << current << ',' << elapsed << ',' << target.instance.pid << ',' << target.instance.created << ','
                    << ReadAcquire(&target.handles.shared->originals) << ',' << ReadAcquire(&target.handles.shared->controls) << ','
                    << target.received << ',' << store.segment(target.attach)->windowRetained << ',' << store.size() << ','
                    << method.completed << ',' << method.failed << ',' << loss.overwritten << ',' << loss.writeRejected << ','
                    << loss.pending << ',' << (!stats.known || loss.unknown || method.unknown) << ',' << withholding << ','
                    << ReadAcquire(&target.handles.shared->finished) << '\n';
            }
        }
        if (current >= nextSample) nextSample = current + static_cast<uint64_t>(frequency.QuadPart) / 5;
        Sleep(1);
    }
    if (gui && done) {
        const auto finishDeadline = tick() + static_cast<uint64_t>(frequency.QuadPart) * 20;
        while (!std::filesystem::exists(directory / "gui-final.flag")) {
            if (std::filesystem::exists(directory / "owner-cancel.flag") || tick() >= finishDeadline) return 50;
            Sleep(10);
        }
    }
    std::ofstream result(directory / "result.json");
    result << std::boolalpha << "{\"synthetic_only\":true,\"production_hook_entry_points\":true,\"real_injection\":false,"
        << "\"gui_owns_receiver\":" << gui << ",\"start_qpc\":" << start
        << ",\"qpc_frequency\":" << frequency.QuadPart << ",\"warmup_seconds\":" << warmup << ",\"formal_seconds\":" << seconds
        << ",\"pump_transport_capacity\":" << ipc::kPumpCap << ",\"control_transport_capacity\":" << ipc::kCap
        << ",\"host_window_capacity\":" << kRecent << ",\"ring_bytes\":" << sizeof(ipc::Ring) << ",\"stall\":" << stall
        << ",\"stall_start_qpc\":" << stallStart << ",\"stall_end_qpc\":" << stallEnd
        << ",\"stall_source_before\":" << stalledOriginalsStart << ",\"stall_source_after\":" << stalledOriginalsEnd
        << ",\"other_received_before\":" << otherReceivedStart << ",\"other_received_after\":" << otherReceivedEnd << ",\"targets\":[";
    for (unsigned i = 0; i < targets.size(); ++i) {
        auto& target = targets[i];
        if (!gui) consume(i);
        const auto& shared = *target.handles.shared;
        const auto statistics = ipc::readMethodStatistics(*target.handles.ring, kGeneration);
        const auto& method = statistics.methods[static_cast<unsigned>(probeMethod(i))];
        const auto loss = target.window.transportStatus(*target.handles.ring);
        wa::call_log_filter::FilterOpts query;
        query.attach = target.attach;
        const auto view = store.view(query);
        size_t checked = 0;
        for (size_t n = 0; n < view.size(); ++n) {
            const auto* record = view.at(n);
            if (record->id.lane != wa::CallLane::Pump) continue;
            if (checked >= latest[i].size() || wa::CallLogStore::toJsonLine(record->call) != latest[i][checked] ||
                record->id.sequence != target.received - latest[i].size() + checked + 1) ++target.invalid;
            windows << wa::CallLogStore::toJsonLine(*record) << '\n';
            ++checked;
        }
        const uint64_t originals = static_cast<uint64_t>(ReadAcquire(&shared.originals));
        const auto formalCalls = originals - static_cast<uint64_t>(shared.warmOriginals);
        const double elapsed = static_cast<double>(shared.formalEnd - shared.formalStart) / frequency.QuadPart;
        const double rate = elapsed > 0 ? formalCalls / elapsed : 0;
        const double minimum = load == L"overload" ? (i ? 100.0 : 20000.0) : 2000.0;
        ok = done && elapsed >= seconds && rate >= minimum && !target.invalid && checked == (gui ? 0 : kRecent) &&
            method.completed == originals && !method.unknown && statistics.known &&
            method.failed == 0 && !method.discontinuities &&
            (gui || target.controlReceived == static_cast<uint64_t>(ReadAcquire(&shared.controls))) &&
            (!memory || (store.size() == 0 && ReadAcquire(&shared.controls) == 0)) && ok;
        if (stall && i == 0 && !loss.overwritten && !loss.writeRejected) ok = false;
        if (stall && (!stallEnd || stalledOriginalsEnd - stalledOriginalsStart <= ipc::kPumpCap ||
            otherReceivedEnd <= otherReceivedStart)) ok = false;
        if (!probeSet(target.handles.resume) || probeWait(target.process.hProcess, 10000) != WAIT_OBJECT_0) return 45;
        target.exited = true;
        ok = probeObserved("GetExitCodeProcess", GetExitCodeProcess(target.process.hProcess, &target.exitCode) != FALSE,
                           "pid=" + std::to_string(target.instance.pid)) && target.exitCode == 0 && ok;
        result << (i ? "," : "") << "{\"pid\":" << target.instance.pid << ",\"created_filetime\":" << target.instance.created
            << ",\"requested_rate\":" << rates[i] << ",\"formal_calls\":" << formalCalls << ",\"formal_elapsed_s\":" << elapsed
            << ",\"actual_rate\":" << rate << ",\"formal_controls\":" << ReadAcquire(&shared.controls) - shared.warmControls
            << ",\"originals\":" << originals << ",\"source_completed\":" << method.completed << ",\"source_failed\":" << method.failed
            << ",\"source_known\":" << statistics.known << ",\"received\":" << target.received << ",\"latest_checked\":" << checked
            << ",\"invalid\":" << target.invalid << ",\"overwritten\":" << loss.overwritten << ",\"rejected\":" << loss.writeRejected
            << ",\"pending\":" << loss.pending << ",\"unknown\":" << loss.unknown << ",\"exit\":" << target.exitCode << "}";
    }
    result << "],\"success\":" << ok << "}\n";
    samples.close(); windows.close(); result.close();
    return ok && samples && windows && result ? 0 : 46;
}
}
