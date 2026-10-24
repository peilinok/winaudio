#include "CallLogExportJob.h"
#include "Log.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace wa {
namespace {
std::wstring uniqueSuffix() {
    static std::atomic<uint64_t> sequence{0};
    return L"-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
        L"-" + std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
}

Result fileResult(const char* operation, bool ok, DWORD error) {
    const auto hr = ok ? S_OK : HRESULT_FROM_WIN32(error);
    WA_LOG(ok ? log::Level::Debug : log::Level::Warn, "CallLogExportJob", operation, "export", log::hrName(hr));
    return ok ? Result::Ok() : Result::Fail(hr, std::string(operation) + " failed: " + log::hrName(hr));
}

class FileWriter : public CallLogWriter {
public:
    ~FileWriter() override { if (handle_ != INVALID_HANDLE_VALUE) close(); }
    Result open(const std::wstring& destination) override {
        if (handle_ != INVALID_HANDLE_VALUE)
            return Result::Fail(E_UNEXPECTED, "previous export handle remains open");
        final_ = destination;
        temporary_ = destination + uniqueSuffix() + L".tmp";
        std::error_code error;
        const auto parent = std::filesystem::path(destination).parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, error);
            WA_LOG(error ? log::Level::Warn : log::Level::Debug, "CallLogExportJob", "create_directories",
                   "export parent", error ? error.message() : "ok");
        }
        if (error) return Result::Fail(HRESULT_FROM_WIN32(error.value()), "create export directory failed");
        handle_ = CreateFileW(temporary_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
        const DWORD code = handle_ == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
        const auto result = fileResult("CreateFileW", handle_ != INVALID_HANDLE_VALUE, code);
        created_ = static_cast<bool>(result);
        return result;
    }
    Result write(std::string_view bytes) override {
        while (!bytes.empty()) {
            const DWORD length = static_cast<DWORD>((std::min)(bytes.size(), size_t{MAXDWORD}));
            DWORD written = 0;
            const BOOL ok = WriteFile(handle_, bytes.data(), length, &written, nullptr);
            const DWORD code = ok ? (written ? ERROR_SUCCESS : ERROR_WRITE_FAULT) : GetLastError();
            WA_LOG(log::Level::Trace, "CallLogExportJob", "WriteFile", std::to_string(length), std::to_string(written));
            if (!ok || written == 0) return fileResult("WriteFile", false, code);
            bytes.remove_prefix(written);
        }
        return Result::Ok();
    }
    Result flush() override {
        const BOOL ok = FlushFileBuffers(handle_);
        return fileResult("FlushFileBuffers", ok != FALSE, ok ? ERROR_SUCCESS : GetLastError());
    }
    Result close() override {
        const BOOL ok = CloseHandle(handle_);
        const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
        if (ok) handle_ = INVALID_HANDLE_VALUE;
        return fileResult("CloseHandle", ok != FALSE, code);
    }
    Result publish() override {
        const BOOL ok = MoveFileExW(temporary_.c_str(), final_.c_str(), MOVEFILE_WRITE_THROUGH);
        const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
        if (ok) created_ = false;
        return fileResult("MoveFileExW", ok != FALSE, code);
    }
    Result cleanup() override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            auto closed = close();
            if (!closed) closed.message += "; temporary file: " + std::filesystem::path(temporary_).u8string();
            if (!closed) return closed;
        }
        if (!created_) return Result::Ok();
        const BOOL ok = DeleteFileW(temporary_.c_str());
        const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
        if (ok) created_ = false;
        auto result = fileResult("DeleteFileW", ok != FALSE, code);
        if (!result) result.message += "; temporary file: " + std::filesystem::path(temporary_).u8string();
        return result;
    }
private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    bool created_ = false;
    std::wstring final_, temporary_;
};
}

CallLogExportJob::CallLogExportJob(std::shared_ptr<CallLogWriter> writer)
    : writer_(writer ? std::move(writer) : std::make_shared<FileWriter>()) {}

CallLogExportJob::~CallLogExportJob() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

Result CallLogExportJob::start(CallLogExport snapshot, std::wstring destination, bool csv) {
    if (busy()) return Result::Fail(E_PENDING, "export is busy");
    if (snapshot.records.empty()) return Result::Fail(E_INVALIDARG, "recording range is empty");
    if (destination.empty()) {
        const auto dir = CallLogStore::defaultDirectory();
        if (dir.empty()) return Result::Fail(E_FAIL, "default export directory unavailable");
        const auto stem = CallLogStore::exportFileStem(snapshot);
        destination = dir + L"\\" + std::wstring(stem.begin(), stem.end()) + uniqueSuffix() +
            (csv ? L".csv" : L".jsonl");
    }
    done_.store(false, std::memory_order_relaxed);
    cancelled_.store(false, std::memory_order_relaxed);
    progress_.store(0, std::memory_order_relaxed);
    try {
        worker_ = std::thread(&CallLogExportJob::run, this, std::move(snapshot), std::move(destination), csv);
    } catch (const std::exception& error) {
        return Result::Fail(E_FAIL, error.what());
    }
    return Result::Ok();
}

void CallLogExportJob::run(CallLogExport snapshot, std::wstring destination, bool csv) {
    log::setThreadName("export");
    WA_LOG(log::Level::Info, "CallLogExportJob", "start", std::to_string(snapshot.records.size()), "saving");
    const auto start = std::chrono::steady_clock::now();
    CallLogExportResult outcome;
    outcome.path = std::move(destination);
    bool opened = false, published = false;
    try {
        outcome.result = writer_->open(outcome.path);
        opened = static_cast<bool>(outcome.result);
        if (outcome.result && csv && !cancelled_.load(std::memory_order_relaxed))
            outcome.result = writer_->write(CallLogStore::toCsvHeader() + '\n');
        for (const auto& ref : snapshot.records) {
            if (!outcome.result || cancelled_.load(std::memory_order_relaxed)) break;
            const auto line = (csv ? CallLogStore::toCsvLine(ref->call) : CallLogStore::toJsonLine(*ref)) + '\n';
            outcome.result = writer_->write(line);
            if (outcome.result) progress_.store(++outcome.recordsWritten, std::memory_order_relaxed);
        }
        if (outcome.result && !cancelled_.load(std::memory_order_relaxed)) outcome.result = writer_->flush();
        if (opened) {
            const auto closed = writer_->close();
            if (outcome.result && !closed) outcome.result = closed;
        }
        outcome.cancelled = cancelled_.load(std::memory_order_relaxed);
        if (outcome.result && !outcome.cancelled) {
            outcome.result = writer_->publish();
            published = static_cast<bool>(outcome.result);
        }
    } catch (const std::exception& error) {
        outcome.result = Result::Fail(E_FAIL, error.what());
    }
    if (!published) {
        if (outcome.cancelled && outcome.result) outcome.result = Result::Fail(E_ABORT, "export cancelled");
        const auto cleaned = writer_->cleanup();
        outcome.cleanupFailed = !cleaned;
        if (!cleaned) {
            outcome.result.ok = false;
            outcome.result.message += "; temporary cleanup incomplete: " + cleaned.message;
        }
    }
    outcome.elapsedUs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
    WA_LOG(log::Level::Info, "CallLogExportJob", "finish", std::to_string(outcome.recordsWritten),
           outcome.result ? "published" : outcome.result.message);
    result_ = std::move(outcome);
    done_.store(true, std::memory_order_release);
}

bool CallLogExportJob::take(CallLogExportResult& result) {
    if (!busy() || !done_.load(std::memory_order_acquire)) return false;
    return wait(result);
}

bool CallLogExportJob::wait(CallLogExportResult& result) {
    if (!busy()) return false;
    worker_.join();
    result = std::move(result_);
    return true;
}
}  // namespace wa
