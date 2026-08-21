#pragma once
#include "AudioFormat.h"
#include "ComUtil.h"
#include "Result.h"
#include "StreamParams.h"
#include <audioclient.h>
#include <mmdeviceapi.h>

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
    virtual HRESULT isFormatSupported(AUDCLNT_SHAREMODE shareMode,
                                      const WAVEFORMATEX* format) = 0;
    virtual HRESULT getDevicePeriod(REFERENCE_TIME* defaultPeriod,
                                    REFERENCE_TIME* minimumPeriod) = 0;
    virtual HRESULT getBufferSize(UINT32* frames) = 0;
    virtual HRESULT rebuild() = 0;
    // Client properties sit beside Stream init, not inside it. Default is
    // "not implemented" so Stream init fakes stay unchanged.
    virtual HRESULT setClientProperties(const AudioClientProperties& props) {
        (void)props;
        return E_NOTIMPL;
    }
    virtual HRESULT isOffloadCapable(AUDIO_STREAM_CATEGORY category, BOOL* capable) {
        (void)category;
        if (capable) *capable = FALSE;
        return E_NOTIMPL;
    }
};

class AudioClientInitAdapter : public AudioClientInit {
public:
    AudioClientInitAdapter(ComPtr<IAudioClient>& client, IMMDevice* device);
    HRESULT getMixFormat(WAVEFORMATEX** mix) override;
    HRESULT initialize(AUDCLNT_SHAREMODE shareMode, DWORD streamFlags,
                       REFERENCE_TIME bufferDuration, REFERENCE_TIME periodicity,
                       const WAVEFORMATEX* format) override;
    HRESULT isFormatSupported(AUDCLNT_SHAREMODE shareMode,
                              const WAVEFORMATEX* format) override;
    HRESULT getDevicePeriod(REFERENCE_TIME* defaultPeriod,
                            REFERENCE_TIME* minimumPeriod) override;
    HRESULT getBufferSize(UINT32* frames) override;
    HRESULT rebuild() override;
    HRESULT setClientProperties(const AudioClientProperties& props) override;
    HRESULT isOffloadCapable(AUDIO_STREAM_CATEGORY category, BOOL* capable) override;
private:
    ComPtr<IAudioClient>& client_;
    IMMDevice* device_ = nullptr;
};

enum class StreamInitDirection : uint8_t { Capture, Render };

struct StreamInitRequest {
    const AudioFormat* requested = nullptr; // null = mix (Shared) or exclusive candidates
    StreamParams params{};
    uint32_t extraFlags = 0;
    StreamInitDirection direction = StreamInitDirection::Capture;
};

struct StreamInitOutcome {
    AudioFormat actualFormat{};
    uint32_t frameBytes = 0;
};

Result streamInitShared(AudioClientInit& client, const StreamInitRequest& req,
                        StreamInitOutcome& out);
Result streamInitExclusive(AudioClientInit& client, const StreamInitRequest& req,
                           StreamInitOutcome& out);
Result streamInit(AUDCLNT_SHAREMODE shareMode, AudioClientInit& client,
                  const StreamInitRequest& req, StreamInitOutcome& out);

} // namespace wa
