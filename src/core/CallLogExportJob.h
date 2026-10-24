#pragma once
#include "CallLogStore.h"
#include <atomic>
#include <string_view>
#include <thread>

namespace wa {

// Narrow file boundary; production owns one temporary file beside the destination.
class CallLogWriter {
public:
    virtual ~CallLogWriter() = default;
    virtual Result open(const std::wstring& destination) = 0;
    virtual Result write(std::string_view bytes) = 0;
    virtual Result flush() = 0;
    virtual Result close() = 0;
    virtual Result publish() = 0;
    virtual Result cleanup() = 0;
};

struct CallLogExportResult {
    Result result;
    std::wstring path;
    bool cancelled = false, cleanupFailed = false;
    size_t recordsWritten = 0;
    uint64_t elapsedUs = 0;
};

// Main-thread start/take/wait; the worker touches only frozen owning references.
class CallLogExportJob {
public:
    explicit CallLogExportJob(std::shared_ptr<CallLogWriter> writer = {});
    ~CallLogExportJob();
    Result start(CallLogExport snapshot, std::wstring destination = {}, bool csv = false);
    bool busy() const { return worker_.joinable(); }
    bool take(CallLogExportResult& result);
    bool wait(CallLogExportResult& result);
    void cancel() { cancelled_.store(true, std::memory_order_relaxed); }
    size_t progress() const { return progress_.load(std::memory_order_relaxed); }
private:
    void run(CallLogExport snapshot, std::wstring destination, bool csv);
    std::shared_ptr<CallLogWriter> writer_;
    std::thread worker_;
    std::atomic<bool> done_{false}, cancelled_{false};
    std::atomic<size_t> progress_{0};
    CallLogExportResult result_;
};
}  // namespace wa
