#include "WavFile.h"
#include <algorithm>
#include <cstring>
#include <io.h>
#include <limits>

namespace wa {

namespace {
struct FourCC { char c[4]; };
bool eq(const char a[4], const char* b) { return std::memcmp(a, b, 4) == 0; }
template <typename T> bool readPod(FILE* f, T& v) {
    return std::fread(&v, sizeof(T), 1, f) == 1;
}
bool writeBytes(FILE* f, const void* data, size_t bytes) {
    return bytes == 0 || std::fwrite(data, 1, bytes, f) == bytes;
}
bool tellFile(FILE* f, int64_t& offset) {
    offset = _ftelli64(f);
    return offset >= 0;
}
unsigned bitCount(uint32_t value) {
    unsigned count = 0;
    while (value != 0) {
        count += value & 1u;
        value >>= 1;
    }
    return count;
}
Result validateWavFormat(const AudioFormat& fmt) {
    if (fmt.sampleRate == 0 || fmt.channels == 0 || fmt.channels > 8)
        return Result::Fail(-1, "WavWriter: invalid sample rate or channel count");
    if (fmt.bitsPerSample != 8 && fmt.bitsPerSample != 16 &&
        fmt.bitsPerSample != 24 && fmt.bitsPerSample != 32)
        return Result::Fail(-1, "WavWriter: unsupported PCM container size");
    if (fmt.isFloat && fmt.bitsPerSample != 32)
        return Result::Fail(-1, "WavWriter: only float32 is supported");
    if (fmt.validBits() == 0 || fmt.validBits() > fmt.bitsPerSample)
        return Result::Fail(-1, "WavWriter: invalid valid-bits value");
    if (fmt.channelMask != 0 && bitCount(fmt.channelMask) != fmt.channels)
        return Result::Fail(-1, "WavWriter: channel mask does not match channel count");
    if (fmt.blockAlign() == 0 || fmt.blockAlign() > (std::numeric_limits<uint16_t>::max)())
        return Result::Fail(-1, "WavWriter: invalid block alignment");
    return Result::Ok();
}
}

WavWriter::~WavWriter() { close(); }

Result WavWriter::open(const std::wstring& path, const AudioFormat& fmt) {
    if (file_) {
        if (Result result = close(); !result) return result;
    }
    if (Result valid = validateWavFormat(fmt); !valid) return valid;
    fmt_ = fmt;
    dataBytes_ = 0;
    riffSizeOffset_ = 0;
    dataSizeOffset_ = 0;
    factSampleOffset_ = 0;
    dataStartOffset_ = 0;
    pending_.clear();
    error_ = Result::Ok();
    if (_wfopen_s(&file_, path.c_str(), L"wb") != 0 || !file_)
        return Result::Fail(-1, "WavWriter: cannot open file for write");

    auto failOpen = [&]() {
        (void)std::fclose(file_);
        file_ = nullptr;
        return Result::Fail(-1, "WavWriter: failed to write WAV header");
    };
    const intptr_t osHandle = _get_osfhandle(_fileno(file_));
    if (osHandle == -1 ||
        GetFileType(reinterpret_cast<HANDLE>(osHandle)) != FILE_TYPE_DISK)
        return failOpen();

    const uint32_t zero = 0;
    const uint32_t standardMask = defaultChannelMask(fmt.channels);
    const bool useExtensible = fmt.channels > 2 || fmt.validBits() != fmt.bitsPerSample ||
        (fmt.channelMask != 0 && fmt.channelMask != standardMask);

    if (!writeBytes(file_, "RIFF", 4) || !tellFile(file_, riffSizeOffset_) ||
        riffSizeOffset_ != 4 || !writeBytes(file_, &zero, sizeof(zero)) ||
        !writeBytes(file_, "WAVE", 4) || !writeBytes(file_, "fmt ", 4))
        return failOpen();
    if (useExtensible) {
        const uint32_t fmtChunkSize = 40;
        const WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(fmt);
        if (!writeBytes(file_, &fmtChunkSize, sizeof(fmtChunkSize)) ||
            !writeBytes(file_, &wfx, sizeof(wfx)))
            return failOpen();
    } else {
        const uint32_t fmtChunkSize = 16;
        const uint16_t fmtTag = fmt.isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
        const uint16_t blockAlign = static_cast<uint16_t>(fmt.blockAlign());
        const uint32_t avg = fmt.avgBytesPerSec();
        if (!writeBytes(file_, &fmtChunkSize, sizeof(fmtChunkSize)) ||
            !writeBytes(file_, &fmtTag, sizeof(fmtTag)) ||
            !writeBytes(file_, &fmt.channels, sizeof(fmt.channels)) ||
            !writeBytes(file_, &fmt.sampleRate, sizeof(fmt.sampleRate)) ||
            !writeBytes(file_, &avg, sizeof(avg)) ||
            !writeBytes(file_, &blockAlign, sizeof(blockAlign)) ||
            !writeBytes(file_, &fmt.bitsPerSample, sizeof(fmt.bitsPerSample)))
            return failOpen();
    }
    if (fmt.isFloat) {
        const uint32_t factSize = 4;
        if (!writeBytes(file_, "fact", 4) ||
            !writeBytes(file_, &factSize, sizeof(factSize)) ||
            !tellFile(file_, factSampleOffset_) ||
            !writeBytes(file_, &zero, sizeof(zero)))
            return failOpen();
    }
    if (!writeBytes(file_, "data", 4) || !tellFile(file_, dataSizeOffset_) ||
        !writeBytes(file_, &zero, sizeof(zero)) ||
        !tellFile(file_, dataStartOffset_) || dataStartOffset_ <= 8)
        return failOpen();
    return Result::Ok();
}

void WavWriter::setStdioBuffer(size_t bytes) {
    if (file_ && bytes > 0)
        std::setvbuf(file_, nullptr, _IOFBF, bytes);
}

Result WavWriter::write(const void* data, size_t bytes) {
    if (!file_) return Result::Fail(-1, "WavWriter: file is not open");
    if (!error_) return error_;
    if (bytes == 0) return Result::Ok();
    if (!data) return Result::Fail(-1, "WavWriter: null data");
    const size_t frameBytes = fmt_.blockAlign();
    if (frameBytes == 0) return Result::Fail(-1, "WavWriter: invalid block alignment");

    const auto* src = static_cast<const uint8_t*>(data);
    size_t remaining = bytes;
    auto writeFrames = [&](const uint8_t* frameData, size_t frameDataBytes) -> bool {
        const uint64_t projected = dataBytes_ + frameDataBytes;
        const uint64_t totalSize = static_cast<uint64_t>(dataStartOffset_) + projected
                                 + (projected & 1u);
        if (totalSize < 8 || totalSize - 8 > (std::numeric_limits<uint32_t>::max)()) {
            error_ = Result::Fail(-1, "WavWriter: RIFF size limit exceeded");
            return false;
        }
        if (std::fwrite(frameData, 1, frameDataBytes, file_) != frameDataBytes) {
            error_ = Result::Fail(-1, "WavWriter: file write failed");
            return false;
        }
        dataBytes_ = projected;
        return true;
    };

    if (!pending_.empty()) {
        const size_t take = (std::min)(frameBytes - pending_.size(), remaining);
        pending_.insert(pending_.end(), src, src + take);
        src += take;
        remaining -= take;
        if (pending_.size() == frameBytes) {
            if (!writeFrames(pending_.data(), pending_.size())) return error_;
            pending_.clear();
        }
    }

    const size_t completeBytes = remaining - (remaining % frameBytes);
    if (completeBytes > 0) {
        if (!writeFrames(src, completeBytes)) return error_;
        src += completeBytes;
        remaining -= completeBytes;
    }
    pending_.assign(src, src + remaining);
    return Result::Ok();
}

Result WavWriter::close() {
    if (!file_) return Result::Ok();
    Result result = error_;
    auto recordFailure = [&](const char* message) {
        if (result) result = Result::Fail(-1, message);
    };
    if (!pending_.empty() && result) {
        result = Result::Fail(-1, "WavWriter: incomplete audio frame");
    }
    pending_.clear();
    if (dataBytes_ & 1u) {
        const uint8_t pad = 0;
        if (!writeBytes(file_, &pad, sizeof(pad)))
            recordFailure("WavWriter: failed to write data padding");
    }
    if (std::fflush(file_) != 0)
        recordFailure("WavWriter: failed to flush file");
    int64_t fileSize = -1;
    if (!tellFile(file_, fileSize) || fileSize < 8)
        recordFailure("WavWriter: failed to determine file size");
    const uint32_t riffSize = fileSize >= 8
        ? static_cast<uint32_t>(fileSize - 8) : 0;
    const uint32_t dataSize = static_cast<uint32_t>(dataBytes_);
    if (_fseeki64(file_, riffSizeOffset_, SEEK_SET) != 0 ||
        !writeBytes(file_, &riffSize, sizeof(riffSize)))
        recordFailure("WavWriter: failed to patch RIFF size");
    if (_fseeki64(file_, dataSizeOffset_, SEEK_SET) != 0 ||
        !writeBytes(file_, &dataSize, sizeof(dataSize)))
        recordFailure("WavWriter: failed to patch data size");
    if (factSampleOffset_ != 0) {
        const uint32_t frames = fmt_.blockAlign()
            ? static_cast<uint32_t>(dataBytes_ / fmt_.blockAlign()) : 0;
        if (_fseeki64(file_, factSampleOffset_, SEEK_SET) != 0 ||
            !writeBytes(file_, &frames, sizeof(frames)))
            recordFailure("WavWriter: failed to patch fact sample count");
    }
    if (std::fclose(file_) != 0)
        recordFailure("WavWriter: failed to close file");
    file_ = nullptr;
    return result;
}

WavReader::~WavReader() { close(); }

Result WavReader::open(const std::wstring& path) {
    if (file_) close();
    fmt_ = AudioFormat{};
    remaining_ = 0;
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
            if (sz < 16) {
                close();
                return Result::Fail(-1, "WavReader: bad fmt chunk size");
            }
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
            if (fmtTag == WAVE_FORMAT_EXTENSIBLE) {
                if (sz < 40) {
                    close();
                    return Result::Fail(-1, "WavReader: truncated extensible fmt chunk");
                }
                uint16_t cbSize = 0, validBits = 0;
                uint32_t channelMask = 0;
                GUID subFormat{};
                if (!readPod(file_, cbSize) || !readPod(file_, validBits) ||
                    !readPod(file_, channelMask) || !readPod(file_, subFormat)) {
                    close();
                    return Result::Fail(-1, "WavReader: bad extensible fmt chunk");
                }
                if (cbSize < 22 || static_cast<uint32_t>(cbSize) + 18u > sz) {
                    close();
                    return Result::Fail(-1, "WavReader: invalid extensible cbSize");
                }
                if (subFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
                    fmt_.isFloat = true;
                } else if (subFormat == KSDATAFORMAT_SUBTYPE_PCM) {
                    fmt_.isFloat = false;
                } else {
                    close();
                    return Result::Fail(-1, "WavReader: unsupported extensible subformat");
                }
                fmt_.validBitsPerSample = validBits;
                fmt_.channelMask = channelMask;
                consumed = 40;
            } else {
                if (fmtTag != WAVE_FORMAT_PCM && fmtTag != WAVE_FORMAT_IEEE_FLOAT) {
                    close();
                    return Result::Fail(-1, "WavReader: unsupported wave format");
                }
                fmt_.isFloat = (fmtTag == WAVE_FORMAT_IEEE_FLOAT);
            }
            if ((sz > consumed &&
                 _fseeki64(file_, static_cast<int64_t>(sz - consumed), SEEK_CUR) != 0) ||
                ((sz & 1u) && _fseeki64(file_, 1, SEEK_CUR) != 0)) {
                close();
                return Result::Fail(-1, "WavReader: failed to skip fmt extension");
            }
            if (fmt_.blockAlign() != blockAlign || fmt_.avgBytesPerSec() != avg ||
                !validateWavFormat(fmt_)) {
                close();
                return Result::Fail(-1, "WavReader: inconsistent audio format");
            }
            haveFmt = true;
        } else if (eq(id, "data")) {
            remaining_ = sz;
            haveData = true;
        } else {
            if (_fseeki64(file_, static_cast<int64_t>(sz) + (sz & 1u), SEEK_CUR) != 0) {
                close();
                return Result::Fail(-1, "WavReader: failed to skip chunk");
            }
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
    Result result = Result::Ok();
    if (file_) {
        if (std::fclose(file_) != 0)
            result = Result::Fail(-1, "WavReader: failed to close file");
        file_ = nullptr;
    }
    remaining_ = 0;
    return result;
}

} // namespace wa
