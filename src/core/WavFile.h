#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include "AudioFormat.h"
#include "Result.h"

namespace wa {

class WavWriter {
public:
    ~WavWriter();
    Result open(const std::wstring& path, const AudioFormat& fmt);
    void   setStdioBuffer(size_t bytes);          // setvbuf; no-op if not open
    size_t write(const void* data, size_t bytes); // returns bytes written
    Result close();                               // patches sizes
private:
    FILE*  file_ = nullptr;
    uint32_t dataBytes_ = 0;
    AudioFormat fmt_{};
};

class WavReader {
public:
    ~WavReader();
    Result open(const std::wstring& path);
    const AudioFormat& format() const { return fmt_; }
    size_t read(void* out, size_t bytes);  // returns bytes read (0 at EOF)
    bool   eof() const { return remaining_ == 0; }
    Result close();
private:
    FILE*  file_ = nullptr;
    uint32_t remaining_ = 0;   // bytes left in data chunk
    AudioFormat fmt_{};
};

} // namespace wa
