#include "Log.h"

#include <spdlog/spdlog.h>
#include <spdlog/async.h>
#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/fmt/fmt.h>

#include <windows.h>
#include <audioclient.h>

#include <memory>
#include <mutex>

namespace wa::log {
namespace {

constexpr const char* kPattern   = "%H:%M:%S.%e %v";
constexpr size_t      kQueueSize = 8192;

std::shared_ptr<spdlog::logger> g_logger;
std::mutex                      g_sinkMutex;
thread_local const char*        t_threadName = "?";

spdlog::level::level_enum toSpd(Level l) {
    switch (l) {
        case Level::Trace: return spdlog::level::trace;
        case Level::Debug: return spdlog::level::debug;
        case Level::Info:  return spdlog::level::info;
        case Level::Warn:  return spdlog::level::warn;
        case Level::Err:   return spdlog::level::err;
    }
    return spdlog::level::info;
}

Level fromSpd(spdlog::level::level_enum l) {
    switch (l) {
        case spdlog::level::trace:    return Level::Trace;
        case spdlog::level::debug:    return Level::Debug;
        case spdlog::level::info:     return Level::Info;
        case spdlog::level::warn:     return Level::Warn;
        case spdlog::level::err:      return Level::Err;
        case spdlog::level::critical: return Level::Err;
        case spdlog::level::off:      return Level::Err;
        case spdlog::level::n_levels: return Level::Info;
    }
    return Level::Info;
}

const char* levelStr(Level l) {
    switch (l) {
        case Level::Trace: return "TRACE";
        case Level::Debug: return "DEBUG";
        case Level::Info:  return "INFO";
        case Level::Warn:  return "WARN";
        case Level::Err:   return "ERROR";
    }
    return "INFO";
}

// Static table of the HRESULTs this tool actually encounters. Returns nullptr
// for anything not listed (callers fall back to the system message or hex).
const char* hrNameC(long hr) {
    switch (static_cast<HRESULT>(hr)) {
        case S_OK:                                return "S_OK";
        case S_FALSE:                             return "S_FALSE";
        case E_INVALIDARG:                        return "E_INVALIDARG";
        case E_POINTER:                           return "E_POINTER";
        case E_NOINTERFACE:                       return "E_NOINTERFACE";
        case E_OUTOFMEMORY:                       return "E_OUTOFMEMORY";
        case E_FAIL:                              return "E_FAIL";
        case E_ACCESSDENIED:                      return "E_ACCESSDENIED";
        case RPC_E_CHANGED_MODE:                  return "RPC_E_CHANGED_MODE";
        case AUDCLNT_E_NOT_INITIALIZED:           return "AUDCLNT_E_NOT_INITIALIZED";
        case AUDCLNT_E_ALREADY_INITIALIZED:       return "AUDCLNT_E_ALREADY_INITIALIZED";
        case AUDCLNT_E_WRONG_ENDPOINT_TYPE:       return "AUDCLNT_E_WRONG_ENDPOINT_TYPE";
        case AUDCLNT_E_DEVICE_INVALIDATED:        return "AUDCLNT_E_DEVICE_INVALIDATED";
        case AUDCLNT_E_NOT_STOPPED:               return "AUDCLNT_E_NOT_STOPPED";
        case AUDCLNT_E_BUFFER_TOO_LARGE:          return "AUDCLNT_E_BUFFER_TOO_LARGE";
        case AUDCLNT_E_OUT_OF_ORDER:              return "AUDCLNT_E_OUT_OF_ORDER";
        case AUDCLNT_E_UNSUPPORTED_FORMAT:        return "AUDCLNT_E_UNSUPPORTED_FORMAT";
        case AUDCLNT_E_INVALID_SIZE:              return "AUDCLNT_E_INVALID_SIZE";
        case AUDCLNT_E_DEVICE_IN_USE:             return "AUDCLNT_E_DEVICE_IN_USE";
        case AUDCLNT_E_BUFFER_OPERATION_PENDING:  return "AUDCLNT_E_BUFFER_OPERATION_PENDING";
        case AUDCLNT_E_THREAD_NOT_REGISTERED:     return "AUDCLNT_E_THREAD_NOT_REGISTERED";
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:return "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED";
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED:    return "AUDCLNT_E_ENDPOINT_CREATE_FAILED";
        case AUDCLNT_E_SERVICE_NOT_RUNNING:       return "AUDCLNT_E_SERVICE_NOT_RUNNING";
        case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED:   return "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED";
        case AUDCLNT_S_BUFFER_EMPTY:              return "AUDCLNT_S_BUFFER_EMPTY";
        default:                                  return nullptr;
    }
}

// Sink that forwards each formatted line to a user callback (GUI panel).
template <typename Mutex>
class CallbackSink : public spdlog::sinks::base_sink<Mutex> {
public:
    explicit CallbackSink(std::function<void(Level, const std::string&)> cb)
        : cb_(std::move(cb)) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t buf;
        this->formatter_->format(msg, buf);
        cb_(fromSpd(msg.level), fmt::to_string(buf));
    }
    void flush_() override {}

private:
    std::function<void(Level, const std::string&)> cb_;
};

void ensureLogger() {
    if (g_logger) return;
    spdlog::init_thread_pool(kQueueSize, 1);
    g_logger = std::make_shared<spdlog::async_logger>(
        "wa", spdlog::sinks_init_list{}, spdlog::thread_pool(),
        spdlog::async_overflow_policy::overrun_oldest);
    g_logger->set_pattern(kPattern);
    g_logger->set_level(spdlog::level::info);
}

void pushSink(const spdlog::sink_ptr& sink) {
    std::lock_guard<std::mutex> lk(g_sinkMutex);
    ensureLogger();
    sink->set_pattern(kPattern);
    g_logger->sinks().push_back(sink);
}

} // namespace

void setThreadName(const char* name) { t_threadName = name ? name : "?"; }

void init() {
    std::lock_guard<std::mutex> lk(g_sinkMutex);
    ensureLogger();
}

void shutdown() {
    // Must not throw: called from CLI's LogShutdown destructor (a sink flush
    // failure must not escape into the destructor path).
    try {
        if (g_logger) g_logger->flush();
        spdlog::shutdown();
    } catch (...) {
    }
    g_logger.reset();
}

void setLevel(Level lvl) {
    std::lock_guard<std::mutex> lk(g_sinkMutex);
    ensureLogger();
    g_logger->set_level(toSpd(lvl));
}

Level getLevel() {
    std::lock_guard<std::mutex> lk(g_sinkMutex);
    if (!g_logger) return Level::Info;  // a query has no side effects: don't create a logger
    return fromSpd(g_logger->level());
}

bool shouldLog(Level lvl) {
    return g_logger && g_logger->should_log(toSpd(lvl));
}

void addFileSink(const std::string& path, size_t maxBytes, size_t maxFiles) {
    pushSink(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(path, maxBytes, maxFiles));
}

void addStderrSink() {
    pushSink(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
}

void addCallbackSink(std::function<void(Level, const std::string&)> cb) {
    pushSink(std::make_shared<CallbackSink<std::mutex>>(std::move(cb)));
}

void emit(Level lvl, const char* module, const char* call,
          const std::string& args, const std::string& ret) {
    if (!g_logger) return;
    const std::string modcall = std::string(module) + "::" + call;
    const std::string payload =
        fmt::format("{:<5} [{:<4}] {:<46} args: {:<38} ret: {}", levelStr(lvl),
                    t_threadName, modcall, args.empty() ? std::string("-") : args, ret);
    g_logger->log(toSpd(lvl), payload);
}

void emitTrace(const char* module, const char* call,
               unsigned frames, unsigned flags, long hr) {
    if (!g_logger) return;
    // Audio thread: no heap allocation here — spdlog formats into an inline
    // stack buffer (memory_buf_t, 250B; these trace lines are well under that)
    // and enqueues with overrun_oldest (drops the oldest rather than waiting on
    // a full queue); hrNameC returns a static string. NOTE: spdlog's async queue
    // is an mpmc_blocking_queue, so the enqueue takes a short mutex — Trace is a
    // diagnostic aid that may perturb the audio thread slightly (see spec).
    const char* sym = hrNameC(hr);
    g_logger->trace("TRACE [{:<4}] {}::{} frames={} flags={:#x} hr={:#010x} {}",
                    t_threadName, module, call, frames, flags,
                    static_cast<unsigned long>(hr), sym ? sym : "");
}

std::string hrName(long hr) {
    if (const char* s = hrNameC(hr)) return s;
    std::string out = fmt::format("{:#010x}", static_cast<unsigned long>(hr));
    LPSTR buf = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), 0, reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    if (n && buf) {
        std::string msg(buf, n);
        while (!msg.empty() &&
               (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' '))
            msg.pop_back();
        if (!msg.empty()) out += " " + msg;
    }
    if (buf) LocalFree(buf);
    return out;
}

} // namespace wa::log
