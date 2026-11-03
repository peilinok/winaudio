#include "CallLogStore.h"
#include "HookedCall.h"
#include "Log.h"
#include <cstdio>
#include <algorithm>
#include <numeric>
#include <fstream>
#include <sstream>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

namespace wa {
namespace {

std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    sprintf_s(buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
        }
    }
    return out;
}

std::string csvEscape(const std::string& s) {
    bool need = false;
    for (char c : s) {
        if (c == '"' || c == ',' || c == '\n' || c == '\r') {
            need = true;
            break;
        }
    }
    if (!need) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += "\"\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

Result writeRecords(const std::wstring& path, const CallLogExport& snapshot, bool csv) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f)
        return Result::Fail(static_cast<long>(HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)),
                            "CallLogStore: open failed");
    bool ok = true;
    const auto writeLine = [&](const std::string& line) {
        return fwrite(line.data(), 1, line.size(), f) == line.size() && fputc('\n', f) != EOF;
    };
    if (csv) ok = writeLine(CallLogStore::toCsvHeader());
    for (const auto& record : snapshot.records) {
        if (!ok) break;
        ok = writeLine(csv ? CallLogStore::toCsvLine(record->call) : CallLogStore::toJsonLine(*record));
    }
    if (fflush(f) != 0) ok = false;
    if (fclose(f) != 0) ok = false;
    return ok ? Result::Ok() : Result::Fail(static_cast<long>(E_FAIL), "CallLogStore: write/close failed");
}

bool ensureDir(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    return CreateDirectoryW(dir.c_str(), nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}

}  // namespace

void CallLogStore::clear() { beginRound(); }

bool CallLogStore::clearSegment(uint64_t attach, uint32_t generation, int64_t startedMs) {
    if (!attach || attach > attaches_.size()) return false;
    auto& target = attaches_[static_cast<size_t>(attach - 1)];
    if (target.active && generation <= target.observationGeneration) return false;
    const auto oldFrozenCount = frozenCount_;
    size_t removedFrozen = 0;
    for (size_t index = 0; index < entries_.size(); ++index)
        if (entries_[index]->id.attach == attach && index < oldFrozenCount) ++removedFrozen;
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [attach](const CallRecordRef& ref) {
        return ref->id.attach == attach;
    }), entries_.end());
    frozenCount_ -= removedFrozen;
    pidCounts_[target.pid] -= target.count;
    target.count = target.windowRetained = 0;
    target.windowReceived = target.windowEvicted = 0;
    const bool unknownControl = target.transport.control.unknown;
    const bool unknownPump = target.transport.pump.unknown;
    const auto snapshotSequence = target.transport.snapshotSequence;
    target.transport = {};
    target.transport.control.unknown = unknownControl;
    target.transport.pump.unknown = unknownPump;
    target.transport.snapshotSequence = snapshotSequence;
    target.statisticsStartedMs = startedMs;
    target.observationGeneration = generation;
    target.contributors.clear();
    target.hint = {};
    target.hintRevision = kNotFound;
    target.pump.clear();
    target.controlIfaces.clear();
    controlIfaces_.clear();
    for (const auto& ref : entries_) call_log_filter::addIface(controlIfaces_, ref->call.iface);
    ++controlRevision_;
    queryValid_ = false;
    combinedRevision_ = combinedLegacyRevision_ = kNotFound;
    rebuildPump();
    return true;
}

void CallLogStore::beginRound() {
    entries_.clear();
    attaches_.clear();
    latestAttach_.clear();
    tokenAttach_.clear();
    pidCounts_.clear();
    pump_.clear();
    ++controlRevision_;
    ++pumpRevision_;
    sorted_.clear();
    matched_.clear();
    positions_.clear();
    matchedPump_.clear();
    pumpPositions_.clear();
    controlIfaces_.clear();
    ifaces_.clear();
    queryValid_ = false;
    combinedAttach_ = combinedLegacy_ = 0;
    combinedRevision_ = combinedLegacyRevision_ = kNotFound;
    ++round_;
    nextControlSequence_ = 0;
    range_ = recording_ ? RecordingRange::Following : RecordingRange::Never;
    frozenCount_ = 0;
}

uint64_t CallLogStore::registerAttach(uint32_t pid) {
    if (const auto active = activeAttach(pid)) return active;
    AttachSegment segment;
    segment.pid = pid;
    segment.instance.pid = pid;
    segment.id = attaches_.size() + 1;
    attaches_.push_back(std::move(segment));
    const uint64_t id = attaches_.size();
    latestAttach_[pid] = id;
    return id;
}

uint64_t CallLogStore::registerAttach(ProcessInstance instance, std::string name, uint64_t token, int64_t startedMs,
                                      uint32_t methods) {
    if (!instance || !token) return 0;
    if (const auto existing = attachForToken(token))
        return segment(existing)->instance == instance ? existing : 0;
    if (const auto current = activeAttach(instance.pid)) {
        if (segment(current)->instance == instance) return current;
        // A new instance cannot inherit an old instance's active ownership.
        endAttach(current, true, startedMs);
    }
    AttachSegment next;
    next.id = attaches_.size() + 1;
    next.token = token;
    next.pid = instance.pid;
    next.instance = instance;
    next.name = std::move(name);
    next.startedMs = next.snapshotMs = startedMs;
    next.statisticsStartedMs = startedMs;
    next.initialMethods = next.methods = methods;
    attaches_.push_back(std::move(next));
    const uint64_t id = attaches_.size();
    latestAttach_[instance.pid] = tokenAttach_[token] = id;
    return id;
}

const CallLogStore::Segment* CallLogStore::segment(uint64_t attach) const {
    return attach && attach <= attaches_.size() ? &attaches_[static_cast<size_t>(attach - 1)] : nullptr;
}

bool CallLogStore::setStartup(uint64_t attach, StartupObservation observation) {
    if (!attach || attach > attaches_.size()) return false;
    auto& segment = attaches_[static_cast<size_t>(attach - 1)];
    if (!segment.active || segment.startup) return false;
    segment.startup = std::move(observation);
    return true;
}

bool CallLogStore::setMethods(uint64_t attach, uint32_t methods, int64_t observedMs,
                              std::optional<uint32_t> observedGeneration) {
    if (!attach || attach > attaches_.size() || (methods & ~kAllMonitorMethods)) return false;
    auto& target = attaches_[static_cast<size_t>(attach - 1)];
    if (!target.active) return false;
    if (target.methods != methods)
        target.configurationChanges.push_back({methods, observedGeneration.value_or(target.observationGeneration), observedMs});
    target.methods = methods;
    return true;
}

bool CallLogStore::setObservationGeneration(uint64_t attach, uint32_t generation) {
    if (!attach || attach > attaches_.size()) return false;
    auto& target = attaches_[static_cast<size_t>(attach - 1)];
    if (!target.active || target.observationGeneration) return false;
    target.observationGeneration = generation;
    return true;
}

bool CallLogStore::updateTransport(const TransportUpdate& update) {
    const auto id = attachForToken(update.token);
    if (!id || !update.instance || !update.status.snapshotSequence) return false;
    auto& target = attaches_[static_cast<size_t>(id - 1)];
    if (!target.active || target.instance != update.instance ||
        target.observationGeneration != update.status.observationGeneration ||
        update.status.snapshotSequence <= target.transport.snapshotSequence) return false;
    target.transport = update.status;
    return true;
}

std::vector<CallLogStore::Segment> CallLogStore::segments(bool active) const {
    std::vector<Segment> out;
    for (auto it = attaches_.rbegin(); it != attaches_.rend(); ++it)
        if (it->active == active) out.push_back(*it);
    return out;
}

uint64_t CallLogStore::attachForToken(uint64_t token) const {
    const auto found = tokenAttach_.find(token);
    return found == tokenAttach_.end() ? 0 : found->second;
}

uint64_t CallLogStore::latestAttach(ProcessInstance instance) const {
    if (!instance) return 0;
    for (auto it = attaches_.rbegin(); it != attaches_.rend(); ++it)
        if (it->instance == instance) return it->id;
    return 0;
}

const HookedInitializeExtract& CallLogStore::initializeHintForInstance(ProcessInstance instance, CallLogObserver observer) {
    return initializeHint(latestAttach(instance), observer);
}

void CallLogStore::endAttach(uint64_t attach) {
    endAttach(attach, false, 0);
}

void CallLogStore::endAttach(uint64_t attach, bool exited, int64_t snapshotMs) {
    if (attach == 0 || attach > attaches_.size()) return;
    auto& segment = attaches_[static_cast<size_t>(attach - 1)];
    if (!segment.active) return;
    segment.active = false;
    segment.state = exited ? SegmentState::Exited : SegmentState::Detached;
    if (snapshotMs) segment.snapshotMs = snapshotMs;
    rebuildPump();
}

const std::vector<CallRecordRef>& CallLogStore::pumpEntries() const {
    static const std::vector<CallRecordRef> empty;
    return pumpVisible_ ? pump_ : empty;
}

void CallLogStore::setPumpVisible(bool on) {
    if (on == pumpVisible_) return;
    pumpVisible_ = on;
    ++pumpRevision_;
}

void CallLogStore::rebuildPump() {
    ++pumpRevision_;
    pump_.clear();
    for (const auto& segment : attaches_)
        pump_.insert(pump_.end(), segment.pump.begin(), segment.pump.end());
    std::sort(pump_.begin(), pump_.end(), [](const CallRecordRef& a, const CallRecordRef& b) {
        if (a->call.timeMs != b->call.timeMs) return a->call.timeMs < b->call.timeMs;
        if (a->id.attach != b->id.attach) return a->id.attach < b->id.attach;
        return a->id.sequence < b->id.sequence;
    });
}

void CallLogStore::updatePump(uint64_t attach, const std::vector<PumpCall>& window) {
    // Legacy snapshot callers hand off the same completed identities. Only new
    // rows enter the segment's shared window; a smaller snapshot never erases it.
    appendPump(attach, window);
}

void CallLogStore::appendPump(uint64_t attach, const std::vector<PumpCall>& received) {
    if (attach == 0 || attach > attaches_.size()) return;
    auto& segment = attaches_[static_cast<size_t>(attach - 1)];
    if (!segment.active) return;
    bool changed = false;
    for (const auto& observed : received) {
        if (!observed.sequence || (!segment.pump.empty() &&
            observed.sequence <= segment.pump.back()->id.sequence)) continue;
        auto call = observed.call;
        if ((call.pid && call.pid != segment.pid) ||
            call.observationGeneration != segment.observationGeneration ||
            (call.processCreated && call.processCreated != segment.instance.created) ||
            (call.attachToken && call.attachToken != segment.token)) continue;
        call.pid = segment.pid;
        call.processCreated = segment.instance.created;
        call.attachToken = segment.token;
        call.pump = true;
        call.args = sanitizeHookedArgs(std::move(call.args));
        CallRecord record{{round_, attach, observed.sequence, CallLane::Pump}, std::move(call)};
        segment.pump.push_back(std::make_shared<const CallRecord>(std::move(record)));
        ++segment.windowReceived;
        if (segment.pump.size() > kDefaultPumpRingCap) {
            segment.pump.erase(segment.pump.begin());
            ++segment.windowEvicted;
        }
        segment.windowRetained = segment.pump.size();
        changed = true;
    }
    if (changed) rebuildPump();
}

uint64_t CallLogStore::latestAttach(uint32_t pid) const {
    const auto it = latestAttach_.find(pid);
    return it == latestAttach_.end() ? 0 : it->second;
}

uint64_t CallLogStore::activeAttach(uint32_t pid) const {
    const auto id = latestAttach(pid);
    return id && attaches_[static_cast<size_t>(id - 1)].active ? id : 0;
}

void CallLogStore::append(uint64_t attach, HookedCall call) {
    if (attach == 0 || attach > attaches_.size()) return;
    auto& segment = attaches_[static_cast<size_t>(attach - 1)];
    if (!segment.active || call.pump || isPumpMethod(call.method)) return;
    if ((call.attachToken && call.attachToken != segment.token) ||
        call.observationGeneration != segment.observationGeneration ||
        (call.processCreated && call.processCreated != segment.instance.created)) return;
    if (segment.instance && call.pid && call.pid != segment.instance.pid) return;
    if (segment.pid != 0) call.pid = segment.pid;
    call.attachToken = segment.token;
    call.processCreated = segment.instance.created;
    call.args = sanitizeHookedArgs(std::move(call.args));
    CallRecord record{{round_, attach, ++nextControlSequence_, CallLane::Control}, std::move(call)};
    auto ref = std::make_shared<const CallRecord>(std::move(record));
    ++pidCounts_[ref->call.pid];
    call_log_filter::addIface(controlIfaces_, ref->call.iface);
    call_log_filter::addIface(segment.controlIfaces, ref->call.iface);
    ++controlRevision_;
    ++segment.count;
    if (ref->call.method == "Initialize" || ref->call.method == "InitializeSharedAudioStream" ||
        ref->call.method == "SetClientProperties") segment.contributors.push_back(ref);
    entries_.push_back(std::move(ref));
}

const std::vector<std::string>& CallLogStore::ifaces() {
    if (ifacesControlRevision_ == controlRevision_ && ifacesPumpRevision_ == pumpRevision_) return ifaces_;
    ifaces_ = controlIfaces_;
    for (const auto& ref : pumpEntries()) call_log_filter::addIface(ifaces_, ref->call.iface);
    call_log_filter::sortIfaces(ifaces_);
    ifacesControlRevision_ = controlRevision_;
    ifacesPumpRevision_ = pumpRevision_;
    return ifaces_;
}

std::vector<std::string> CallLogStore::ifaces(uint64_t attach) const {
    if (!attach || attach > attaches_.size()) return {};
    const auto& segment = attaches_[static_cast<size_t>(attach - 1)];
    auto result = segment.controlIfaces;
    if (pumpVisible_) for (const auto& ref : segment.pump) call_log_filter::addIface(result, ref->call.iface);
    call_log_filter::sortIfaces(result);
    return result;
}

CallLogStore::View CallLogStore::view(const call_log_filter::FilterOpts& query, CallLogObserver observer) {
    viewWork_ = {};
    viewWork_.controlChanged = matchedRevision_ != controlRevision_;
    viewWork_.queryChanged = !queryValid_ || query.text != queryText_ || query.iface != queryIface_ ||
        query.failuresOnly != queryFailures_ || query.attach != queryAttach_;
    viewWork_.pumpChanged = matchedPumpRevision_ != pumpRevision_;
    if (sortedRevision_ != controlRevision_) {
        observer.stage(CallLogStage::Sort, true, entries_.size());
        sorted_.resize(entries_.size());
        std::iota(sorted_.begin(), sorted_.end(), size_t{0});
        std::sort(sorted_.begin(), sorted_.end(), [&](size_t a, size_t b) {
            if (entries_[a]->call.timeMs != entries_[b]->call.timeMs)
                return entries_[a]->call.timeMs < entries_[b]->call.timeMs;
            return a < b;
        });
        viewWork_.sorted = entries_.size();
        sortedRevision_ = controlRevision_;
        observer.stage(CallLogStage::Sort, false, entries_.size());
    }
    if (viewWork_.controlChanged || viewWork_.queryChanged) {
        observer.stage(CallLogStage::Match, true, entries_.size());
        queryText_ = query.text;
        queryIface_ = query.iface;
        queryFailures_ = query.failuresOnly;
        queryAttach_ = query.attach;
        queryValid_ = true;
        matched_.clear();
        positions_.assign(entries_.size(), kNotFound);
        for (const auto index : sorted_) {
            if (query.attach && entries_[index]->id.attach != query.attach) continue;
            if (!call_log_filter::callMatches(entries_[index]->call, query)) continue;
            positions_[index] = matched_.size();
            matched_.push_back(index);
        }
        viewWork_.matched = entries_.size();
        matchedRevision_ = controlRevision_;
        observer.stage(CallLogStage::Match, false, entries_.size());
    }
    if (viewWork_.controlChanged || viewWork_.queryChanged || viewWork_.pumpChanged) {
        const auto& pump = pumpVisible_ && query.attach && query.attach <= attaches_.size()
            ? attaches_[static_cast<size_t>(query.attach - 1)].pump : pumpEntries();
        observer.stage(CallLogStage::PumpMatch, true, pump.size());
        matchedPump_.clear();
        pumpPositions_.clear();
        for (const auto& ref : pump) {
            if (query.attach && ref->id.attach != query.attach) continue;
            if (!call_log_filter::callMatches(ref->call, query)) continue;
            // Equal timestamps place every control row before pump metadata.
            const auto insertion = std::upper_bound(matched_.begin(), matched_.end(), ref->call.timeMs,
                [&](int64_t time, size_t index) { return time < entries_[index]->call.timeMs; });
            pumpPositions_.push_back(static_cast<size_t>(insertion - matched_.begin()) + matchedPump_.size());
            matchedPump_.push_back(ref.get());
        }
        viewWork_.pumpMatched = pump.size();
        matchedPumpRevision_ = pumpRevision_;
        observer.stage(CallLogStage::PumpMatch, false, pump.size());
    }
    return View(this);
}

size_t CallLogStore::View::size() const { return store_->matched_.size() + store_->matchedPump_.size(); }

const CallRecord* CallLogStore::View::at(size_t position) const {
    if (position >= size()) return nullptr;
    const auto& pump = store_->pumpPositions_;
    const auto it = std::lower_bound(pump.begin(), pump.end(), position);
    const auto before = static_cast<size_t>(it - pump.begin());
    if (it != pump.end() && *it == position) return store_->matchedPump_[before];
    return store_->entries_[store_->matched_[position - before]].get();
}

size_t CallLogStore::View::position(CallRecordId id) const {
    if (id.round != store_->round_ || !id) return kNotFound;
    if (id.lane == CallLane::Pump) {
        for (size_t i = 0; i < store_->matchedPump_.size(); ++i)
            if (store_->matchedPump_[i]->id == id) return store_->pumpPositions_[i];
        return kNotFound;
    }
    const auto index = store_->controlIndex(id);
    if (index == kNotFound || index >= store_->positions_.size()) return kNotFound;
    const auto base = store_->positions_[index];
    if (base == kNotFound) return kNotFound;
    size_t position = base;
    for (size_t i = 0; i < store_->pumpPositions_.size(); ++i) {
        if (store_->pumpPositions_[i] - i > base) break;
        ++position;
    }
    return position;
}

const CallLogViewWork& CallLogStore::View::work() const { return store_->viewWork_; }

size_t CallLogStore::View::indexBytes() const {
    return (store_->sorted_.capacity() + store_->matched_.capacity() + store_->positions_.capacity() +
        store_->pumpPositions_.capacity()) * sizeof(size_t) + store_->matchedPump_.capacity() * sizeof(const CallRecord*);
}

void CallLogStore::append(uint64_t attach, std::vector<HookedCall> calls) {
    for (auto& call : calls) append(attach, std::move(call));
}

void CallLogStore::append(std::vector<HookedCall> calls) {
    for (auto& call : calls) append(registerAttach(call.pid), std::move(call));
}

size_t CallLogStore::controlIndex(CallRecordId id) const {
    if (id.round != round_ || id.lane != CallLane::Control || !id) return kNotFound;
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), id.sequence,
        [](const CallRecordRef& ref, uint64_t sequence) { return ref->id.sequence < sequence; });
    return it != entries_.end() && (*it)->id == id ? static_cast<size_t>(it - entries_.begin()) : kNotFound;
}

const CallRecord* CallLogStore::find(CallRecordId id) const {
    if (id.round == round_ && id.lane == CallLane::Pump) {
        if (pumpVisible_ && id.attach && id.attach <= attaches_.size())
            for (const auto& ref : attaches_[static_cast<size_t>(id.attach - 1)].pump)
                if (ref->id == id) return ref.get();
        return nullptr;
    }
    const auto index = controlIndex(id);
    return index == kNotFound ? nullptr : entries_[index].get();
}

size_t CallLogStore::pidCallCount(uint32_t pid) const {
    const auto it = pidCounts_.find(pid);
    return it == pidCounts_.end() ? 0 : it->second;
}

std::vector<const HookedCall*> CallLogStore::sessionRecords(uint32_t pid) const {
    std::vector<const CallRecord*> records;
    const auto appendSegment = [&](uint64_t id) {
        if (id == 0) return;
        for (const auto& ref : entries_) if (ref->id.attach == id) records.push_back(ref.get());
    };
    appendSegment(latestAttach(pid));
    if (pid != 0) appendSegment(latestAttach(0));
    std::sort(records.begin(), records.end(), [](const CallRecord* a, const CallRecord* b) {
        if (a->call.timeMs != b->call.timeMs) return a->call.timeMs < b->call.timeMs;
        return a->id.sequence < b->id.sequence;
    });
    std::vector<const HookedCall*> out;
    out.reserve(records.size());
    for (const auto* record : records) out.push_back(&record->call);
    return out;
}

size_t CallLogStore::sessionCallCount(uint32_t pid) const {
    const auto count = [&](uint64_t id) { return id ? attaches_[static_cast<size_t>(id - 1)].count : 0; };
    return count(latestAttach(pid)) + (pid != 0 ? count(latestAttach(0)) : 0);
}

const HookedInitializeExtract& CallLogStore::initializeHint(uint64_t attach, CallLogObserver observer) {
    static const HookedInitializeExtract empty;
    if (!attach || attach > attaches_.size()) return empty;
    auto& segment = attaches_[static_cast<size_t>(attach - 1)];
    if (segment.hintRevision == segment.contributors.size()) return segment.hint;
    observer.stage(CallLogStage::Initialize, true, segment.contributors.size());
    std::sort(segment.contributors.begin(), segment.contributors.end(), [](const CallRecordRef& a, const CallRecordRef& b) {
        if (a->call.timeMs != b->call.timeMs) return a->call.timeMs < b->call.timeMs;
        return a->id.sequence < b->id.sequence;
    });
    std::vector<const HookedCall*> refs;
    refs.reserve(segment.contributors.size());
    for (const auto& ref : segment.contributors) refs.push_back(&ref->call);
    segment.hint = extractHookedInitializeRefs(refs);
    segment.hintRevision = segment.contributors.size();
    observer.stage(CallLogStage::Initialize, false, refs.size());
    return segment.hint;
}

const HookedInitializeExtract& CallLogStore::initializeHintForPid(uint32_t pid, CallLogObserver observer) {
    const auto attach = latestAttach(pid);
    const auto legacy = pid != 0 ? latestAttach(0) : 0;
    if (!legacy || attaches_[static_cast<size_t>(legacy - 1)].contributors.empty())
        return initializeHint(attach, observer);
    const auto revision = attach ? attaches_[static_cast<size_t>(attach - 1)].contributors.size() : 0;
    const auto legacyRevision = attaches_[static_cast<size_t>(legacy - 1)].contributors.size();
    if (combinedAttach_ == attach && combinedLegacy_ == legacy && combinedRevision_ == revision &&
        combinedLegacyRevision_ == legacyRevision) return combinedHint_;
    observer.stage(CallLogStage::Initialize, true, revision + legacyRevision);
    std::vector<const CallRecord*> records;
    records.reserve(revision + legacyRevision);
    if (attach) for (const auto& ref : attaches_[static_cast<size_t>(attach - 1)].contributors) records.push_back(ref.get());
    for (const auto& ref : attaches_[static_cast<size_t>(legacy - 1)].contributors) records.push_back(ref.get());
    std::sort(records.begin(), records.end(), [](const CallRecord* a, const CallRecord* b) {
        if (a->call.timeMs != b->call.timeMs) return a->call.timeMs < b->call.timeMs;
        return a->id.sequence < b->id.sequence;
    });
    std::vector<const HookedCall*> refs;
    refs.reserve(records.size());
    for (const auto* record : records) refs.push_back(&record->call);
    combinedHint_ = extractHookedInitializeRefs(refs);
    combinedAttach_ = attach;
    combinedLegacy_ = legacy;
    combinedRevision_ = revision;
    combinedLegacyRevision_ = legacyRevision;
    observer.stage(CallLogStage::Initialize, false, refs.size());
    return combinedHint_;
}

void CallLogStore::setRecording(bool on) {
    if (on == recording_) return;
    recording_ = on;
    range_ = on ? RecordingRange::Following : RecordingRange::Frozen;
    if (!on) frozenCount_ = entries_.size();
}

bool CallLogStore::recording() const {
    return recording_;
}

size_t CallLogStore::size() const {
    return entries_.size();
}

CallLogExport CallLogStore::freezeExport() const {
    const size_t count = exportCount();
    CallLogExport out{round_, count ? entries_[count - 1]->id.sequence : 0, {}};
    out.records.assign(entries_.begin(), entries_.begin() + static_cast<std::ptrdiff_t>(count));
    return out;
}

CallLogExport CallLogStore::freezeExport(uint64_t attach, int64_t frozenMs) const {
    CallLogExport out;
    const auto* selected = segment(attach);
    if (!selected) return out;
    out.round = round_;
    out.segment = *selected;
    out.frozenMs = frozenMs;
    for (const auto& record : entries_) {
        if (record->id.attach != attach) continue;
        out.records.push_back(record);
        out.upperSequence = record->id.sequence;
    }
    const auto& window = attaches_[static_cast<size_t>(attach - 1)].pump;
    out.records.insert(out.records.end(), window.begin(), window.end());
    return out;
}

size_t CallLogStore::exportCount() const {
    return range_ == RecordingRange::Frozen ? frozenCount_ : entries_.size();
}

std::wstring CallLogStore::defaultDirectory() {
    wchar_t* path = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)) || !path)
        return {};
    std::wstring dir = path;
    CoTaskMemFree(path);
    dir += L"\\WinAudio\\pipeline-calls";
    return dir;
}

std::string CallLogStore::defaultFileStem(uint32_t pid) {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char buf[96];
    sprintf_s(buf, "calls-pid%u-%04u%02u%02u-%02u%02u%02u", pid, st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::string CallLogStore::exportFileStem(const CallLogExport& snapshot) {
    if (snapshot.segment)
        return defaultFileStem(snapshot.segment->instance.pid) + "-segment" + std::to_string(snapshot.segment->id);
    const uint32_t pid = snapshot.records.empty() ? 0 : snapshot.records.front()->call.pid;
    std::string stem = defaultFileStem(pid);
    for (const auto& record : snapshot.records) {
        if (record->call.pid == pid) continue;
        stem.replace(6, stem.find('-', 6) - 6, "multi");
        break;
    }
    return stem;
}

std::string CallLogStore::formatHresult(int32_t hr) {
    return wa::log::hrName(hr);
}

std::string CallLogStore::formatHresultHex(int32_t hr) {
    char buf[16];
    sprintf_s(buf, "0x%08X", static_cast<unsigned>(hr));
    return buf;
}

std::string CallLogStore::toJsonLine(const HookedCall& c) {
    std::ostringstream oss;
    oss << "{\"timeMs\":" << c.timeMs << ",\"pid\":" << c.pid << ",\"iface\":\"" << jsonEscape(c.iface) << "\",\"method\":\""
        << jsonEscape(c.method) << "\",\"args\":\"" << jsonEscape(c.args) << "\",\"hr\":\""
        << jsonEscape(formatHresult(c.hresult)) << "\",\"hrHex\":\"" << formatHresultHex(c.hresult)
        << "\",\"hresult\":" << c.hresult << ",\"streamId\":" << c.streamId
        << ",\"pump\":" << (c.pump ? "true" : "false") << ",\"xrun\":" << (c.xrun ? "true" : "false");
    if (c.category) oss << ",\"category\":\"" << jsonEscape(*c.category) << "\"";
    if (c.format) oss << ",\"format\":\"" << jsonEscape(*c.format) << "\"";
    if (c.exclusive) oss << ",\"exclusive\":" << (*c.exclusive ? "true" : "false");
    if (c.raw) oss << ",\"raw\":" << (*c.raw ? "true" : "false");
    if (c.matchFormat) oss << ",\"matchFormat\":" << (*c.matchFormat ? "true" : "false");
    if (!c.parameters.empty()) {
        oss << ",\"parameters\":[";
        for (size_t i = 0; i < c.parameters.size(); ++i) {
            const auto& a = c.parameters[i];
            if (i) oss << ',';
            oss << "{\"depth\":" << static_cast<unsigned>(a.depth) << ",\"dir\":\""
                << callArgDirName(a.dir) << "\",\"type\":\"" << jsonEscape(a.type) << "\",\"name\":\""
                << jsonEscape(a.name) << "\",\"pre\":\"" << jsonEscape(a.pre) << "\",\"post\":\""
                << jsonEscape(a.post) << "\"}";
        }
        oss << ']';
    }
    oss << "}";
    return oss.str();
}

std::string CallLogStore::toCsvHeader(bool segment) {
    std::string header = "timeMs,pid,iface,method,args,hr,hresult,streamId,pump,xrun";
    if (segment) header += ",roundId,attachId,lane,sequence,processCreated,attachToken,observationGeneration,rowType,summaryKey,summaryValue";
    return header;
}

std::string CallLogStore::toJsonLine(const CallRecord& record) {
    std::string line = toJsonLine(record.call);
    line.pop_back();
    line += ",\"roundId\":" + std::to_string(record.id.round) +
        ",\"attachId\":" + std::to_string(record.id.attach) +
        ",\"lane\":\"" + (record.id.lane == CallLane::Control ? "control" : "pump") +
        "\",\"sequence\":" + std::to_string(record.id.sequence) +
        ",\"processCreated\":" + std::to_string(record.call.processCreated) +
        ",\"attachToken\":" + std::to_string(record.call.attachToken) +
        ",\"observationGeneration\":" + std::to_string(record.call.observationGeneration) +
        ",\"rowType\":\"call\"}";
    return line;
}

std::string CallLogStore::toCsvLine(const HookedCall& c) {
    std::ostringstream oss;
    oss << c.timeMs << ',' << c.pid << ',' << csvEscape(c.iface) << ',' << csvEscape(c.method) << ','
        << csvEscape(c.args) << ',' << formatHresult(c.hresult) << ',' << c.hresult << ','
        << c.streamId << ',' << (c.pump ? 1 : 0) << ',' << (c.xrun ? 1 : 0);
    return oss.str();
}

std::string CallLogStore::toCsvLine(const CallRecord& record) {
    return toCsvLine(record.call) + ',' + std::to_string(record.id.round) + ',' +
        std::to_string(record.id.attach) + ',' + (record.id.lane == CallLane::Control ? "control" : "pump") +
        ',' + std::to_string(record.id.sequence) + ',' + std::to_string(record.call.processCreated) +
        ',' + std::to_string(record.call.attachToken) + ',' + std::to_string(record.call.observationGeneration) +
        ",call,,";
}

std::vector<std::string> CallLogStore::exportSummary(const CallLogExport& snapshot, bool csv) {
    if (!snapshot.segment) return {};
    const auto& s = *snapshot.segment;
    // Dotted summary keys have identical meanings in JSONL and flat CSV.
    struct Field { std::string key, value; bool text = false; };
    std::vector<Field> fields;
    const auto number = [&](const std::string& key, auto value) { fields.push_back({key, std::to_string(value)}); };
    const auto boolean = [&](const std::string& key, bool value) { fields.push_back({key, value ? "true" : "false"}); };
    const auto text = [&](const std::string& key, const std::string& value) { fields.push_back({key, value, true}); };
    number("schemaVersion", 2);
    number("roundId", snapshot.round);
    number("attachId", s.id);
    number("pid", s.instance.pid);
    number("processCreated", s.instance.created);
    number("attachToken", s.token);
    text("name", s.name);
    text("state", s.state == SegmentState::Monitoring ? "monitoring" : s.state == SegmentState::Exited ? "exited" : "detached");
    number("startedMs", s.startedMs);
    number("segmentSnapshotMs", s.snapshotMs);
    number("frozenMs", snapshot.frozenMs);
    number("statisticsStartedMs", s.statisticsStartedMs);
    number("observationGeneration", s.observationGeneration);
    boolean("coverageUnknown", s.coverageUnknown);
    text("observationMode", s.startup ? "startup" : "running-attach");
    if (s.startup) {
        const auto& origin = *s.startup;
        text("startup.boundary", "main-thread-exe-pe-entry-after-readiness");
        text("startup.exclusions", "pre-entry-DLL-TLS,prior-helper-threads,unknown-implementations,other-PIDs,forwarded-existing-instance,child-processes");
        text("startup.capabilityScope", "discovered-implementations-at-readiness-not-complete-API-coverage");
        text("startup.hookPath", origin.hookPath);
        number("startup.mainThreadId", origin.mainThreadId);
        number("startup.imageBase", origin.imageBase);
        number("startup.entryAddress", origin.entryAddress);
        number("startup.entryRva", origin.entryRva);
        number("startup.protocolLayout", origin.protocolLayout);
        number("startup.acceptedAttempts", origin.acceptedAttempts);
        number("startup.rejectedAttempts", origin.rejectedAttempts);
        number("startup.capabilityError", origin.capabilityError);
        boolean("startup.receiverReady", origin.receiverReady);
        number("startup.receiverReadyQpc", origin.receiverReadyCounter);
    }
    text("windowScope", "recent-retained-not-complete-history");
    text("configurationTimeScope", "host-observed-success-not-exact-source-publication");
    text("snapshotScope", "host-received-at-click-not-simultaneous-source-snapshot");
    number("methods", s.methods);
    number("initialMethods", s.initialMethods);
    number("configurationChanges.count", s.configurationChanges.size());
    for (size_t i = 0; i < s.configurationChanges.size(); ++i) {
        const auto prefix = "configurationChanges." + std::to_string(i) + '.';
        const auto& change = s.configurationChanges[i];
        number(prefix + "methods", change.methods);
        number(prefix + "observedMs", change.observedMs);
        number(prefix + "observationGeneration", change.observationGeneration);
    }
    number("controlRetained", s.count);
    number("controlUpperSequence", snapshot.upperSequence);
    number("windowRetained", s.windowRetained);
    number("windowReceived", s.windowReceived);
    number("windowEvicted", s.windowEvicted);
    for (const auto lane : {CallLane::Control, CallLane::Pump}) {
        const std::string prefix = lane == CallLane::Control ? "control" : "window";
        bool known = false;
        int64_t first = 0, last = 0;
        uint64_t upper = 0;
        for (const auto& record : snapshot.records) {
            if (record->id.lane != lane) continue;
            if (!known) first = last = record->call.timeMs;
            first = (std::min)(first, record->call.timeMs);
            last = (std::max)(last, record->call.timeMs);
            upper = (std::max)(upper, record->id.sequence);
            known = true;
        }
        boolean(prefix + "RecordTimeKnown", known);
        number(prefix + "FirstRecordMs", first);
        number(prefix + "LastRecordMs", last);
        if (lane == CallLane::Pump) number("windowUpperSequence", upper);
    }
    const auto& t = s.transport;
    number("transport.snapshotMs", t.snapshotMs);
    number("transport.snapshotSequence", t.snapshotSequence);
    number("transport.observationGeneration", t.observationGeneration);
    const auto transport = [&](const std::string& prefix, const HookTransportLaneStatus& status, uint64_t dropped) {
        number(prefix + ".overwritten", status.overwritten);
        number(prefix + ".writeRejected", status.writeRejected);
        number(prefix + ".pending", status.pending);
        boolean(prefix + ".unknown", status.unknown);
        number(prefix + ".controllerDropped", dropped);
    };
    transport("transport.control", t.control, t.controllerDroppedControl);
    transport("transport.pump", t.pump, t.controllerDroppedPump);
    boolean("statistics.known", t.statistics.known);
    for (size_t i = 0; i < static_cast<size_t>(MonitorMethod::Count); ++i) {
        if (kMonitorMethods[i].control) continue;
        const auto prefix = std::string("statistics.") + kMonitorMethods[i].iface + '.' + kMonitorMethods[i].method;
        const auto& method = t.statistics.methods[i];
        number(prefix + ".completed", method.completed);
        number(prefix + ".failed", method.failed);
        number(prefix + ".discontinuities", method.discontinuities);
        boolean(prefix + ".unknown", method.unknown);
    }
    std::vector<std::string> lines;
    if (!csv) {
        std::string line = "{\"rowType\":\"segment\"";
        for (const auto& field : fields)
            line += ",\"" + field.key + "\":" + (field.text ? '"' + jsonEscape(field.value) + '"' : field.value);
        lines.push_back(line + '}');
    } else {
        for (const auto& field : fields) {
            std::vector<std::string> columns(20);
            columns[10] = std::to_string(snapshot.round);
            columns[11] = std::to_string(s.id);
            columns[14] = std::to_string(s.instance.created);
            columns[15] = std::to_string(s.token);
            columns[16] = std::to_string(s.observationGeneration);
            columns[17] = "segment";
            columns[18] = field.key;
            columns[19] = field.value;
            std::string line;
            for (size_t i = 0; i < columns.size(); ++i) {
                if (i) line += ',';
                line += csvEscape(columns[i]);
            }
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

Result CallLogStore::saveJsonl(const std::wstring& path) const {
    return writeRecords(path, freezeExport(), false);
}

Result CallLogStore::saveCsv(const std::wstring& path) const {
    return writeRecords(path, freezeExport(), true);
}

Result CallLogStore::saveDefaultJsonl(std::wstring& outPath) const {
    const std::wstring dir = defaultDirectory();
    if (dir.empty() || !ensureDir(dir))
        return Result::Fail(static_cast<long>(E_FAIL), "CallLogStore: cannot create directory");
    const auto snapshot = freezeExport();
    const std::string stem = exportFileStem(snapshot);
    outPath = dir + L"\\" + std::wstring(stem.begin(), stem.end()) + L".jsonl";
    return writeRecords(outPath, snapshot, false);
}

std::string liveSessionAttachKey(const LiveSessionView& row) {
    if (!row.sessionInstanceId.empty()) return row.sessionInstanceId;
    return std::to_string(row.processId) + "|" + row.deviceId + "|" +
           (row.flow == PipelineFlow::Render ? "R" : "C");
}

const char* pipelineAttachUiStateName(PipelineAttachUiState state) {
    switch (state) {
        case PipelineAttachUiState::Attached: return "Attached";
        case PipelineAttachUiState::Failed: return "Failed";
        case PipelineAttachUiState::Detached:
        default: return "Detached";
    }
}

}  // namespace wa
