#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include "MethodSelection.h"
#include "HookTransportStatus.h"

namespace wa {
namespace hook_ipc {

constexpr uint32_t kMagic = 0x5741484Bu;
constexpr uint32_t kCap = 2048;
constexpr uint32_t kPumpCap = 64;

constexpr uint32_t kInstallPending = 0;
constexpr uint32_t kInstallOk = 1;
constexpr uint32_t kInstallFailed = 2;
// Layout 10 adds generation-tagged source method statistics to layout 9's slots.
constexpr uint32_t kRingLayout = 10;
constexpr uint32_t kCoverageDiscoveredImplementations = 1;
constexpr uint32_t kCoverageInstallationFault = 2;

constexpr uint32_t kArgBlobCap = 640;
constexpr uint8_t kArgBlobVersion = 1;

// IUnknown occupies slots 0-2. These match the Windows SDK vtables.
constexpr int kSlotDeviceActivate = 3;
constexpr int kSlotEnumeratorEnumEndpoints = 3;
constexpr int kSlotEnumeratorGetDefault = 4;
constexpr int kSlotEnumeratorGetDevice = 5;
constexpr int kSlotClientInitialize = 3;
constexpr int kSlotClientStart = 10;
constexpr int kSlotClientStop = 11;
constexpr int kSlotClientReset = 12;
constexpr int kSlotClientSetEventHandle = 13;
constexpr int kSlotClientGetService = 14;
constexpr int kSlotClientSetProperties = 16;
constexpr int kSlotClientInitializeShared = 20;
constexpr int kSlotBufferGetBuffer = 3;
constexpr int kSlotBufferReleaseBuffer = 4;

enum CallArgDir : uint8_t {
    kArgIn = 0,
    kArgOut = 1,
    kArgInOut = 2,
    kArgRet = 3,
};

#ifndef WA_HOOK_PATCH_OBSERVE
#define WA_HOOK_PATCH_OBSERVE(stage, cell) ((void)0)
#endif
#ifndef WA_HOOK_VIRTUAL_PROTECT
#define WA_HOOK_VIRTUAL_PROTECT(address, size, protection, previous) \
    VirtualProtect(address, size, protection, previous)
#endif

struct VtableInstallerState {
    SRWLOCK lock = SRWLOCK_INIT;
    volatile LONG fatalError = ERROR_SUCCESS;
};

static inline VtableInstallerState& vtableInstallerState() {
    static VtableInstallerState state;
    return state;
}

static inline DWORD vtableInstallationError() {
    return static_cast<DWORD>(ReadAcquire(&vtableInstallerState().fatalError));
}

// Installation only. The lock spans protection changes on the entire page;
// forwarding never acquires it. The original is immutable once published.
static inline bool patchVtableSlot(void* obj, int slot, void* hook, void** orig) {
    if (!obj || !hook || !orig) return false;
    auto& state = vtableInstallerState();
    WA_HOOK_PATCH_OBSERVE(before_lock, nullptr);
    AcquireSRWLockExclusive(&state.lock);
    struct Unlock {
        SRWLOCK* lock;
        ~Unlock() { ReleaseSRWLockExclusive(lock); }
    } unlock{&state.lock};
    if (const DWORD error = vtableInstallationError()) {
        SetLastError(error);
        return false;
    }
    void** vt = *reinterpret_cast<void***>(obj);
    void** cell = vt + slot;
    void* current = ReadPointerAcquire(cell);
    void* original = InterlockedCompareExchangePointer(orig, nullptr, nullptr);
    if (current == hook) {
        if (original && original != hook) return true;
        SetLastError(ERROR_INVALID_FUNCTION);
        return false;
    }
    if (!current || (original && original != current)) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    // Only pin implementations accepted under the same installation lock.
    // Unknown/rejected implementations must remain free to unload normally.
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                           reinterpret_cast<LPCWSTR>(current), &module) ||
        !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                            reinterpret_cast<LPCWSTR>(hook), &module)) return false;
    DWORD old = 0;
    if (!WA_HOOK_VIRTUAL_PROTECT(cell, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    WA_HOOK_PATCH_OBSERVE(protected_page, cell);
    InterlockedCompareExchangePointer(orig, current, nullptr);
    InterlockedExchangePointer(cell, hook);
    WA_HOOK_PATCH_OBSERVE(published, cell);
    DWORD tmp = 0;
    if (!WA_HOOK_VIRTUAL_PROTECT(cell, sizeof(void*), old, &tmp)) {
        const DWORD error = GetLastError();
        // Make one immediate cleanup attempt while the owner and lock still
        // guarantee this page's lifetime. Never retain an address for later:
        // its object could be destroyed and the address reused after return.
        if (!WA_HOOK_VIRTUAL_PROTECT(cell, sizeof(void*), old, &tmp))
            InterlockedExchange(&state.fatalError, static_cast<LONG>(error));
        else
            WA_HOOK_PATCH_OBSERVE(restored, cell);
        SetLastError(error);
        return false;
    }
    WA_HOOK_PATCH_OBSERVE(restored, cell);
    return true;
}

// Packed arg blob: [u8 ver=1][u8 count] then count records of
// [u8 depth][u8 dir][u8 tlen][type][u8 nlen][name][u8 plen][pre][u8 qlen][post].
struct ArgBlobWriter {
    uint8_t* buf = nullptr;
    uint16_t cap = 0;
    uint16_t len = 0;
    uint8_t count = 0;

    void reset(uint8_t* b, uint16_t c) {
        buf = b;
        cap = c;
        len = 0;
        count = 0;
        if (cap >= 2) {
            buf[0] = kArgBlobVersion;
            buf[1] = 0;
            len = 2;
        }
    }

    bool push(uint8_t depth, uint8_t dir, const char* type, const char* name, const char* pre,
              const char* post) {
        if (!buf || cap < 2 || len < 2) return false;
        const char* t = type ? type : "";
        const char* n = name ? name : "";
        const char* p = pre ? pre : "";
        const char* q = post ? post : p;
        const size_t tl = strnlen(t, 255);
        const size_t nl = strnlen(n, 255);
        const size_t pl = strnlen(p, 255);
        const size_t ql = strnlen(q, 255);
        const size_t need = 6 + tl + nl + pl + ql;
        if (static_cast<size_t>(len) + need > cap || count == 255) return false;
        buf[len++] = depth;
        buf[len++] = dir;
        buf[len++] = static_cast<uint8_t>(tl);
        if (tl) {
            memcpy(buf + len, t, tl);
            len = static_cast<uint16_t>(len + tl);
        }
        buf[len++] = static_cast<uint8_t>(nl);
        if (nl) {
            memcpy(buf + len, n, nl);
            len = static_cast<uint16_t>(len + nl);
        }
        buf[len++] = static_cast<uint8_t>(pl);
        if (pl) {
            memcpy(buf + len, p, pl);
            len = static_cast<uint16_t>(len + pl);
        }
        buf[len++] = static_cast<uint8_t>(ql);
        if (ql) {
            memcpy(buf + len, q, ql);
            len = static_cast<uint16_t>(len + ql);
        }
        ++count;
        buf[1] = count;
        return true;
    }
};

template <typename Fn>
inline bool decodeArgBlob(const uint8_t* data, uint16_t len, Fn&& fn) {
    if (!data || len < 2 || data[0] != kArgBlobVersion) return false;
    const uint8_t count = data[1];
    uint16_t off = 2;
    for (uint8_t i = 0; i < count; ++i) {
        if (off + 6 > len) return false;
        const uint8_t depth = data[off++];
        const uint8_t dir = data[off++];
        const uint8_t tl = data[off++];
        if (off + tl >= len) return false;
        const char* type = reinterpret_cast<const char*>(data + off);
        off = static_cast<uint16_t>(off + tl);
        const uint8_t nl = data[off++];
        if (off + nl >= len) return false;
        const char* name = reinterpret_cast<const char*>(data + off);
        off = static_cast<uint16_t>(off + nl);
        const uint8_t pl = data[off++];
        if (off + pl >= len) return false;
        const char* pre = reinterpret_cast<const char*>(data + off);
        off = static_cast<uint16_t>(off + pl);
        const uint8_t ql = data[off++];
        if (off + ql > len) return false;
        const char* post = reinterpret_cast<const char*>(data + off);
        off = static_cast<uint16_t>(off + ql);
        fn(depth, dir, type, tl, name, nl, pre, pl, post, ql);
    }
    return true;
}

struct CallPod {
    uint32_t streamId;
    int64_t timeMs;
    char iface[40];
    char method[40];
    char args[192];
    int32_t hresult;
    uint8_t pump;
    uint8_t xrun;
    uint8_t hasCategory;
    uint8_t hasRaw;
    uint8_t hasMatchFormat;
    uint8_t hasExclusive;
    uint8_t hasFormat;
    uint8_t raw;
    uint8_t matchFormat;
    uint8_t exclusive;
    char category[32];
    char format[64];
    uint16_t argBlobLen;
    uint8_t argBlob[kArgBlobCap];
    uint32_t observationGeneration;
};

struct alignas(8) SlotPublication {
    uint64_t ownership;
    uint64_t sequence;
    uint32_t hasSequence;
};

struct alignas(8) LaneCounters {
    uint64_t reserved;
    // Upper word is the observation generation; lower word counts proven loss.
    uint64_t overwritten;
    uint64_t rejected;
    uint64_t unknown;
};

struct alignas(8) MethodCounters {
    uint64_t completed, failed, discontinuities, unknown;
};

struct Ring {
    uint32_t magic;
    uint32_t writeIndex;
    uint32_t cap;
    uint32_t dropped;
    uint32_t pumpEnabled;
    uint32_t pumpWriteIndex;
    uint32_t pumpXruns;
    uint32_t installed;
    uint32_t flow;  // 0 = capture, 1 = render
    uint32_t captureEnabled;  // 0 = detach/quiet; 1 = record calls
    wchar_t deviceId[256];
    CallPod slots[kCap];
    CallPod pumpSlots[kPumpCap];
    uint32_t patchAccepted;  // successful patch attempts, including repeats
    uint32_t patchRejected;
    uint32_t patchLastError;  // Win32 error from the last rejection
    uint32_t coverageFlags;
    uint64_t processCreated;
    alignas(8) uint32_t monitorMask;
    uint32_t observationGeneration;
    uint32_t layout;
    uint32_t layoutBytes;
    uint64_t transportEpoch;
    LaneCounters controlCounters, pumpCounters;
    SlotPublication controlPublication[kCap];
    SlotPublication pumpPublication[kPumpCap];
    MethodCounters methodCounters[static_cast<size_t>(MonitorMethod::Count)];
};

static_assert(offsetof(Ring, monitorMask) % 8 == 0, "Atomic selection must be aligned on x86 too");
inline uint64_t monitorState(const Ring& ring) {
    return static_cast<uint64_t>(InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(const_cast<uint32_t*>(&ring.monitorMask)), 0, 0));
}
inline void publishMonitorState(Ring& ring, uint32_t mask, uint32_t generation) {
    const uint64_t state = (static_cast<uint64_t>(generation) << 32) | mask;
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&ring.monitorMask), static_cast<LONG64>(state));
}

inline uint64_t atomicRead64(const uint64_t& value) {
    return static_cast<uint64_t>(InterlockedCompareExchange64(
        reinterpret_cast<volatile LONG64*>(const_cast<uint64_t*>(&value)), 0, 0));
}
inline void atomicStore64(uint64_t& value, uint64_t desired) {
    InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&value), static_cast<LONG64>(desired));
}
inline uint64_t increment64(uint64_t& value) {
    return static_cast<uint64_t>(InterlockedIncrement64(reinterpret_cast<volatile LONG64*>(&value)));
}
inline uint64_t slotOwner(uint32_t generation, uint32_t state) {
    return (static_cast<uint64_t>(generation) << 32) | state;
}
constexpr uint32_t kSlotEmpty = 0, kSlotWriting = 1, kSlotReady = 2, kSlotReading = 3;

#ifndef WA_HOOK_TRANSPORT_OBSERVE
#define WA_HOOK_TRANSPORT_OBSERVE(stage) ((void)0)
#endif

static inline void countLoss(Ring& ring, uint64_t& counter, LaneCounters& lane, uint32_t generation) {
    // Bounded contention handling. Failure to count is explicitly unknown;
    // an old call can never increment the new segment's counters.
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const auto previous = atomicRead64(counter);
        const auto owner = static_cast<uint32_t>(previous >> 32);
        if (static_cast<uint32_t>(monitorState(ring) >> 32) != generation) return;
        const uint32_t count = owner == generation ? static_cast<uint32_t>(previous) : 0;
        const auto desired = slotOwner(generation, count + 1);
        WA_HOOK_TRANSPORT_OBSERVE(counter_attempt);
        if (static_cast<uint64_t>(InterlockedCompareExchange64(
                reinterpret_cast<volatile LONG64*>(&counter), static_cast<LONG64>(desired),
                static_cast<LONG64>(previous))) == previous) {
            if (count == UINT32_MAX) atomicStore64(lane.unknown, 1);
            return;
        }
    }
    atomicStore64(lane.unknown, 1);
}

// One reservation and one ownership CAS; the target never waits for a reader.
// Both readers and writers own the same cell before touching non-atomic data.
static inline bool writeCall(Ring& ring, const CallPod& pod) {
    auto& counters = pod.pump ? ring.pumpCounters : ring.controlCounters;
    const uint64_t ticket = increment64(counters.reserved) - 1;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(pod.pump ? &ring.pumpWriteIndex : &ring.writeIndex));
    WA_HOOK_TRANSPORT_OBSERVE(reserved);
    const uint32_t cap = pod.pump ? kPumpCap : kCap;
    const auto index = static_cast<uint32_t>(ticket % cap);
    auto& publication = pod.pump ? ring.pumpPublication[index] : ring.controlPublication[index];
    auto& payload = pod.pump ? ring.pumpSlots[index] : ring.slots[index];
    const auto previous = atomicRead64(publication.ownership);
    const auto state = static_cast<uint32_t>(previous);
    const uint64_t owned = slotOwner(pod.observationGeneration, kSlotWriting);
    if ((state != kSlotEmpty && state != kSlotReady) ||
        static_cast<uint64_t>(InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(&publication.ownership),
            static_cast<LONG64>(owned), static_cast<LONG64>(previous))) != previous) {
        if (static_cast<uint32_t>(monitorState(ring) >> 32) == pod.observationGeneration)
            countLoss(ring, counters.rejected, counters, pod.observationGeneration);
        return false;
    }
    WA_HOOK_TRANSPORT_OBSERVE(acquired);
    // An old reservation can acquire the cell after a newer publication. It
    // must not replace it, including after the newer row was already consumed.
    const uint64_t distance = ticket - publication.sequence;
    const bool stale = publication.hasSequence &&
        (distance == 0 || distance >= (uint64_t{1} << 63));
    const bool current = static_cast<uint32_t>(monitorState(ring) >> 32) == pod.observationGeneration;
    if (!current || stale) {
        if (current) countLoss(ring, counters.rejected, counters, pod.observationGeneration);
        atomicStore64(publication.ownership, previous);
        return false;
    }
    if (state == kSlotReady && static_cast<uint32_t>(previous >> 32) == pod.observationGeneration)
        countLoss(ring, counters.overwritten, counters, pod.observationGeneration);
    payload = pod;
    publication.sequence = ticket;
    publication.hasSequence = 1;
    atomicStore64(publication.ownership, slotOwner(pod.observationGeneration, kSlotReady));
    return true;
}

// Consuming read. A failed CAS never touches the shared payload. The release
// to Empty acknowledges receipt, so only unreceived Ready rows count as loss.
static inline bool readCall(Ring& ring, bool pump, uint32_t index, uint32_t generation,
                     CallPod& out, uint64_t& sequence) {
    auto& publication = pump ? ring.pumpPublication[index] : ring.controlPublication[index];
    const uint64_t ready = atomicRead64(publication.ownership);
    if (static_cast<uint32_t>(ready) != kSlotReady) return false;
    const auto owned = slotOwner(static_cast<uint32_t>(ready >> 32), kSlotReading);
    if (static_cast<uint64_t>(InterlockedCompareExchange64(
            reinterpret_cast<volatile LONG64*>(&publication.ownership),
            static_cast<LONG64>(owned), static_cast<LONG64>(ready))) != ready) return false;
    WA_HOOK_TRANSPORT_OBSERVE(reading);
    out = pump ? ring.pumpSlots[index] : ring.slots[index];
    sequence = publication.sequence;
    atomicStore64(publication.ownership, slotOwner(static_cast<uint32_t>(ready >> 32), kSlotEmpty));
    return static_cast<uint32_t>(ready >> 32) == generation && out.observationGeneration == generation;
}

inline void initializeRing(Ring& ring) {
    ZeroMemory(&ring, sizeof(ring));
    ring.magic = kMagic;
    ring.cap = kCap;
    ring.layout = kRingLayout;
    ring.layoutBytes = sizeof(Ring);
    ring.transportEpoch = 1;
}
inline bool compatibleRing(const Ring& ring) {
    return ring.magic == kMagic && ring.cap == kCap && ring.layout == kRingLayout &&
        ring.layoutBytes == sizeof(Ring) && atomicRead64(ring.transportEpoch) != 0;
}

inline MethodStatisticsSnapshot readMethodStatistics(const Ring& ring, uint32_t generation) {
    MethodStatisticsSnapshot snapshot;
    snapshot.known = compatibleRing(ring) && static_cast<uint32_t>(monitorState(ring) >> 32) == generation;
    if (!snapshot.known) return snapshot;
    const auto count = [generation](const uint64_t& value) -> uint64_t {
        const auto tagged = atomicRead64(value);
        return static_cast<uint32_t>(tagged >> 32) == generation ? static_cast<uint32_t>(tagged) : 0;
    };
    // Independent atomic samples, not one transaction with each other or rows.
    for (size_t i = 0; i < snapshot.methods.size(); ++i) {
        const auto& source = ring.methodCounters[i];
        snapshot.methods[i] = {count(source.completed), count(source.failed),
            count(source.discontinuities), count(source.unknown) != 0};
    }
    if (static_cast<uint32_t>(monitorState(ring) >> 32) != generation) snapshot.known = false;
    return snapshot;
}

#ifndef WA_HOOK_STATISTICS_OBSERVE
#define WA_HOOK_STATISTICS_OBSERVE(stage, counter) ((void)0)
#endif

static inline void countMethodCompletion(Ring& ring, MonitorMethod method, uint32_t generation,
                                  HRESULT hr, bool discontinuity) {
    auto& counters = ring.methodCounters[static_cast<size_t>(method)];
    const auto unknown = [&] {
        // Attach generations strictly increase; UINT32_MAX rejects reattach.
        // Markers never move backwards. At most one older delayed writer can
        // win against the first observed value: other old writers could not
        // have observed its later value before this generation began. Retry
        // once for that case; a same-generation winner already marks unknown.
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            const auto previous = atomicRead64(counters.unknown);
            if (static_cast<uint32_t>(monitorState(ring) >> 32) != generation ||
                static_cast<uint32_t>(previous >> 32) > generation) return;
            const auto desired = slotOwner(generation, 1);
            if (previous == desired) return;
            WA_HOOK_STATISTICS_OBSERVE(unknown_attempt, &counters.unknown);
            if (static_cast<uint64_t>(InterlockedCompareExchange64(
                    reinterpret_cast<volatile LONG64*>(&counters.unknown),
                    static_cast<LONG64>(desired), static_cast<LONG64>(previous))) == previous) return;
        }
    };
    const auto increment = [&](uint64_t& value) {
        for (unsigned attempt = 0; attempt < 8; ++attempt) {
            const auto previous = atomicRead64(value);
            if (static_cast<uint32_t>(monitorState(ring) >> 32) != generation) return;
            const uint32_t count = static_cast<uint32_t>(previous >> 32) == generation
                ? static_cast<uint32_t>(previous) : 0;
            if (count == UINT32_MAX) { unknown(); return; }
            WA_HOOK_STATISTICS_OBSERVE(count_attempt, &value);
            if (static_cast<uint64_t>(InterlockedCompareExchange64(
                    reinterpret_cast<volatile LONG64*>(&value),
                    static_cast<LONG64>(slotOwner(generation, count + 1)),
                    static_cast<LONG64>(previous))) == previous) return;
        }
        unknown();
    };
    increment(counters.completed);
    if (FAILED(hr)) increment(counters.failed);
    if (discontinuity) increment(counters.discontinuities);
}

static_assert(alignof(SlotPublication) >= 8 && sizeof(SlotPublication) == 24, "Shared slot ABI");
static_assert(offsetof(Ring, controlCounters) % 8 == 0, "Shared counters must be aligned");
static_assert(offsetof(Ring, controlPublication) % 8 == 0, "Shared ownership must be aligned");
static_assert(sizeof(MethodCounters) == 32 && offsetof(Ring, methodCounters) % 8 == 0,
              "Shared method statistics must be aligned on both architectures");

inline void mapName(uint32_t pid, wchar_t* out, size_t n) {
    swprintf_s(out, n, L"Local\\WinAudioHook-%u-%u", kRingLayout, pid);
}

// Everyone + Low mandatory label so an elevated GUI mapping is visible to a
// medium-IL target (and the reverse). CreateFileMapping copies the SD.
inline HANDLE createHookMapping(uint32_t pid, bool* sdApplied = nullptr) {
    wchar_t name[64] = {};
    mapName(pid, name, 64);
    PSECURITY_DESCRIPTOR sd = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    const bool converted = ConvertStringSecurityDescriptorToSecurityDescriptorW(
                               L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &sd, nullptr) !=
                           FALSE;
    if (sdApplied) *sdApplied = converted;
    if (converted) sa.lpSecurityDescriptor = sd;
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, converted ? &sa : nullptr, PAGE_READWRITE, 0,
                                  static_cast<DWORD>(sizeof(Ring)), name);
    const DWORD err = GetLastError();
    if (sd) LocalFree(sd);
    SetLastError(err);
    return h;
}

}  // namespace hook_ipc
}  // namespace wa

