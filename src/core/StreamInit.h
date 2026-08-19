#pragma once
#include "AudioFormat.h"
#include "Result.h"
#include "StreamParams.h"
#include <audioclient.h>

namespace wa {

// Narrow WASAPI Initialize seam used by Stream init. Production wraps IAudioClient;
// tests use a recording fake. Not IAudioBackend.
class AudioClientInit {
public:
    virtual ~AudioClientInit() = default;
    virtual HRESULT getMixFormat(WAVEFORMATEX** mix) = 0;
    virtual HRESULT initialize(AUDCLNT_SHAREMODE shareMode, DWORD streamFlags,
                               REFERENCE_TIME bufferDuration, REFERENCE_TIME periodicity,
                               const WAVEFORMATEX* format) = 0;
};

class AudioClientInitAdapter : public AudioClientInit {
public:
    explicit AudioClientInitAdapter(IAudioClient* client);
    HRESULT getMixFormat(WAVEFORMATEX** mix) override;
    HRESULT initialize(AUDCLNT_SHAREMODE shareMode, DWORD streamFlags,
                       REFERENCE_TIME bufferDuration, REFERENCE_TIME periodicity,
                       const WAVEFORMATEX* format) override;
private:
    IAudioClient* client_ = nullptr;
};

struct StreamInitRequest {
    const AudioFormat* requested = nullptr; // null = use mix format
    StreamParams params{};
    uint32_t extraFlags = 0;
};

struct StreamInitOutcome {
    AudioFormat actualFormat{};
    uint32_t frameBytes = 0;
};

Result streamInitShared(AudioClientInit& client, const StreamInitRequest& req,
                        StreamInitOutcome& out);

} // namespace wa
