#pragma once
#include <string>
#include <functional>

// wa::log — a thin logging facade over spdlog. The implementation (Log.cpp) is
// the only translation unit that includes spdlog, so the rest of core stays
// spdlog-free in its headers and instrumentation sites only depend on this.
// Design: docs/superpowers/specs/2026-07-06-winaudio-verbose-logging-design.md
namespace wa::log {

enum class Level { Trace, Debug, Info, Warn, Err };

// Per-thread short name shown in the [..] column (e.g. "pump", "capW").
// Call once at each thread's entry; defaults to "?" if unset.
void setThreadName(const char* name);

// Initialize the async logger (overrun_oldest overflow policy → never blocks a
// caller). Idempotent. No output is produced until a sink is added.
void init();
// Flush the queue and stop the background pump; call once before process exit.
void shutdown();

void  setLevel(Level lvl);
Level getLevel();
bool  shouldLog(Level lvl);   // hot path calls this first to short-circuit

// Sinks may coexist (file + stderr + GUI callback all at once).
void addFileSink(const std::string& path,
                 size_t maxBytes = 10u * 1024u * 1024u, size_t maxFiles = 3);
void addStderrSink();
// Feeds each formatted line to cb (used by the GUI panel). cb runs on the
// logging pump thread — it must be thread-safe and must not block.
void addCallbackSink(std::function<void(Level, const std::string&)> cb);

// Control-path record (synchronous). Prefer the WA_LOG macro so the args/ret
// strings are only built when the level is enabled.
void emit(Level lvl, const char* module, const char* call,
          const std::string& args, const std::string& ret);

// Hot-path record (audio thread): integer payload only, no std::string
// allocation on the caller — spdlog formats into a stack buffer and enqueues
// without blocking.
void emitTrace(const char* module, const char* call,
               unsigned frames, unsigned flags, long hr);

// HRESULT → symbolic name ("S_OK", "AUDCLNT_E_DEVICE_INVALIDATED", …), falling
// back to the system message text, then a bare hex string. Never empty.
std::string hrName(long hr);

} // namespace wa::log

// Short-circuits arg/ret construction when the level is disabled.
#define WA_LOG(lvl, mod, call, args, ret)                              \
    do {                                                               \
        if (::wa::log::shouldLog(lvl))                                 \
            ::wa::log::emit((lvl), (mod), (call), (args), (ret));      \
    } while (0)
