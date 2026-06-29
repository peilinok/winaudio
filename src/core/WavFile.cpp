#include "WavFile.h"
#include <cstring>

namespace wa {

namespace {
struct FourCC { char c[4]; };
bool eq(const char a[4], const char* b) { return std::memcmp(a, b, 4) == 0; }
template <typename T> bool readPod(FILE* f, T& v) {
    return std::fread(&v, sizeof(T), 1, f) == 1;
}
}

WavWriter::~WavWriter() { close(); }

Result WavWriter::open(const std::wstring& path, const AudioFormat& fmt) {
    if (file_) close();
    fmt_ = fmt;
    dataBytes_ = 0;
    if (_wfopen_s(&file_, path.c_str(), L"wb") != 0 || !file_)
        return Result::Fail(-1, "WavWriter: cannot open file for write");

    // Reserve header; sizes patched on close().
    const uint32_t zero = 0;
    const uint16_t fmtTag = fmt.isFloat ? 3 /*IEEE_FLOAT*/ : 1 /*PCM*/;
    const uint16_t ch = fmt.channels;
    const uint32_t sr = fmt.sampleRate;
    const uint16_t bps = fmt.bitsPerSample;
    const uint16_t blockAlign = static_cast<uint16_t>(fmt.blockAlign());
    const uint32_t avg = fmt.avgBytesPerSec();
    const uint32_t fmtChunkSize = 16;

    std::fwrite("RIFF", 1, 4, file_);
    std::fwrite(&zero, 4, 1, file_);          // RIFF size (patched)
    std::fwrite("WAVE", 1, 4, file_);
    std::fwrite("fmt ", 1, 4, file_);
    std::fwrite(&fmtChunkSize, 4, 1, file_);
    std::fwrite(&fmtTag, 2, 1, file_);
    std::fwrite(&ch, 2, 1, file_);
    std::fwrite(&sr, 4, 1, file_);
    std::fwrite(&avg, 4, 1, file_);
    std::fwrite(&blockAlign, 2, 1, file_);
    std::fwrite(&bps, 2, 1, file_);
    std::fwrite("data", 1, 4, file_);
    std::fwrite(&zero, 4, 1, file_);          // data size (patched)
    return Result::Ok();
}

size_t WavWriter::write(const void* data, size_t bytes) {
    if (!file_) return 0;
    size_t n = std::fwrite(data, 1, bytes, file_);
    dataBytes_ += static_cast<uint32_t>(n);
    return n;
}

Result WavWriter::close() {
    if (!file_) return Result::Ok();
    std::fflush(file_);
    const uint32_t riffSize = 36 + dataBytes_;
    std::fseek(file_, 4, SEEK_SET);  std::fwrite(&riffSize, 4, 1, file_);
    std::fseek(file_, 40, SEEK_SET); std::fwrite(&dataBytes_, 4, 1, file_);
    std::fclose(file_);
    file_ = nullptr;
    return Result::Ok();
}

WavReader::~WavReader() { close(); }

Result WavReader::open(const std::wstring& path) {
    if (file_) close();
    if (_wfopen_s(&file_, path.c_str(), L"rb") != 0 || !file_)
        return Result::Fail(-1, "WavReader: cannot open file for read");

    char tag[4]; uint32_t riffSize;
    if (std::fread(tag, 1, 4, file_) != 4 || !eq(tag, "RIFF") ||
        !readPod(file_, riffSize) ||
        std::fread(tag, 1, 4, file_) != 4 || !eq(tag, "WAVE")) {
        close();
        return Result::Fail(-1, "WavReader: not a RIFF/WAVE file");
    }

    bool haveFmt = false, haveData = false;
    while (!haveData) {
        char id[4]; uint32_t sz;
        if (std::fread(id, 1, 4, file_) != 4 || !readPod(file_, sz)) {
            close();
            return Result::Fail(-1, "WavReader: truncated or missing data chunk");
        }
        if (eq(id, "fmt ")) {
            uint16_t fmtTag, ch, blockAlign, bps; uint32_t sr, avg;
            if (!readPod(file_, fmtTag) || !readPod(file_, ch) ||
                !readPod(file_, sr) || !readPod(file_, avg) ||
                !readPod(file_, blockAlign) || !readPod(file_, bps)) {
                close();
                return Result::Fail(-1, "WavReader: bad fmt chunk");
            }
            fmt_.channels = ch;
            fmt_.sampleRate = sr;
            fmt_.bitsPerSample = bps;
            uint32_t consumed = 16;
            if (fmtTag == 0xFFFE /* WAVE_FORMAT_EXTENSIBLE */ && sz >= 40) {
                // Layout after the 16 standard bytes: cbSize(2) wValidBitsPerSample(2)
                // dwChannelMask(4) SubFormat GUID(16). The GUID's Data1 (first 4 bytes)
                // is 1 for PCM, 3 for IEEE_FLOAT.
                uint16_t cbSize = 0, validBits = 0;
                uint32_t channelMask = 0, subData1 = 0;
                if (!readPod(file_, cbSize) || !readPod(file_, validBits) ||
                    !readPod(file_, channelMask) || !readPod(file_, subData1)) {
                    close();
                    return Result::Fail(-1, "WavReader: bad extensible fmt chunk");
                }
                fmt_.isFloat = (subData1 == 3);
                consumed = 28; // 16 + cbSize(2) + validBits(2) + channelMask(4) + Data1(4)
            } else {
                fmt_.isFloat = (fmtTag == 3);
            }
            if (sz > consumed) std::fseek(file_, static_cast<long>(sz - consumed), SEEK_CUR);
            haveFmt = true;
        } else if (eq(id, "data")) {
            remaining_ = sz;
            haveData = true;
        } else {
            std::fseek(file_, static_cast<long>(sz + (sz & 1)), SEEK_CUR); // word-align
        }
    }
    if (!haveFmt) { close(); return Result::Fail(-1, "WavReader: missing fmt chunk"); }
    return Result::Ok();
}

size_t WavReader::read(void* out, size_t bytes) {
    if (!file_ || remaining_ == 0) return 0;
    size_t want = bytes < remaining_ ? bytes : remaining_;
    size_t n = std::fread(out, 1, want, file_);
    remaining_ -= static_cast<uint32_t>(n);
    return n;
}

Result WavReader::close() {
    if (file_) { std::fclose(file_); file_ = nullptr; }
    remaining_ = 0;
    return Result::Ok();
}

} // namespace wa
