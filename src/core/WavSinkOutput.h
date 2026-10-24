#pragma once
#include <functional>
#include <memory>
#include "AudioFormat.h"
#include "Result.h"

namespace wa {

// File-system boundary; all calls run on the sink's writer thread.
class WavSinkOutput {
public:
    virtual ~WavSinkOutput() = default;
    virtual Result open(const std::wstring& path, const AudioFormat& fmt) = 0;
    virtual Result setBuffer(size_t bytes) = 0;
    virtual Result write(const void* data, size_t bytes) = 0;
    virtual Result close() = 0;
};

using WavSinkOutputFactory = std::function<std::unique_ptr<WavSinkOutput>()>;

} // namespace wa
