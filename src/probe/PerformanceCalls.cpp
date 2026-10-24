#include "PerformanceCalls.h"
#include <cstdio>

namespace wa {
std::vector<HookedCall> performanceCalls(size_t count, unsigned pidCount) {
    std::vector<HookedCall> calls;
    if (!pidCount) return calls;
    calls.reserve(count);
    const char* methods[] = {"Initialize", "GetBufferSize", "GetStreamLatency", "GetCurrentPadding",
                             "SetClientProperties", "InitializeSharedAudioStream", "GetService",
                             "Start", "Stop", "Reset"};
    for (size_t i = 0; i < count; ++i) {
        HookedCall call;
        call.pid = 42001 + static_cast<uint32_t>(i % pidCount);
        const size_t local = i / pidCount;
        call.streamId = 1 + static_cast<uint32_t>((local / 10) % 3);
        call.timeMs = 100000 + static_cast<int64_t>(i / 4) - (i % 19 == 18 ? 10 : 0);
        call.iface = local % 10 == 5 ? "IAudioClient3" : "IAudioClient";
        call.method = methods[local % 10];
        call.hresult = i % 17 == 16 ? static_cast<int32_t>(0x80004005u) : (i % 31 == 30 ? 1 : 0);
        char sequence[32]{};
        std::snprintf(sequence, sizeof(sequence), "fixture=%08zu", i);
        call.args = std::string(sequence) +
            " ShareMode=SHARED StreamFlags=0x80000000 fmt=48000/32f/8\n"
            "buffer=100000 period=10000 category=Media raw=false match=false";
        const std::string hr = call.hresult == 0 ? "S_OK" : (call.hresult == 1 ? "S_FALSE" : "E_FAIL");
        call.parameters = {
            {0, 0, "AUDCLNT_SHAREMODE", "ShareMode", "AUDCLNT_SHAREMODE_SHARED", "AUDCLNT_SHAREMODE_SHARED"},
            {0, 0, "WAVEFORMATEX*", "pFormat", "0x0000000012345678", "0x0000000012345678"},
            {1, 0, "WORD", "wFormatTag", "WAVE_FORMAT_EXTENSIBLE", "WAVE_FORMAT_EXTENSIBLE"},
            {1, 0, "WORD", "nChannels", "8", "8"},
            {1, 0, "DWORD", "nSamplesPerSec", "48000", "48000"},
            {1, 0, "DWORD", "nAvgBytesPerSec", "1536000", "1536000"},
            {1, 0, "WORD", "nBlockAlign", "32", "32"},
            {2, 0, "GUID", "SubFormat", "KSDATAFORMAT_SUBTYPE_IEEE_FLOAT", "KSDATAFORMAT_SUBTYPE_IEEE_FLOAT"},
            {1, 0, "WORD", "wBitsPerSample", "32", "32"},
            {1, 0, "WORD", "cbSize", "22", "22"},
            {0, 0, "DWORD", "StreamFlags", "AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM", "AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM"},
            {0, 0, "REFERENCE_TIME", "hnsBufferDuration", "100000", "100000"},
            {1, 0, "AUDIO_STREAM_CATEGORY", "eCategory", "AudioCategory_Media", "AudioCategory_Media"},
            {1, 0, "AUDCLNT_STREAMOPTIONS", "Options", "AUDCLNT_STREAMOPTIONS_NONE", "AUDCLNT_STREAMOPTIONS_NONE"},
            {0, 3, "HRESULT", "Return", "", hr},
            {0, 2, "wchar_t*", "Description", "input line one\ninput line two", "output line one\noutput line two"}
        };
        if (local % 10 == 0 || local % 10 == 5) {
            call.format = "48000/32f/8";
            call.exclusive = false;
        }
        if (local % 10 == 4) {
            call.category = "Media";
            call.raw = false;
            call.matchFormat = false;
        }
        calls.push_back(std::move(call));
    }
    return calls;
}
}
