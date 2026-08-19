#include "StreamInit.h"
#include "ComUtil.h"
#include "Log.h"
#include "AudioFormatStr.h"

namespace wa {

AudioClientInitAdapter::AudioClientInitAdapter(IAudioClient* client) : client_(client) {}

HRESULT AudioClientInitAdapter::getMixFormat(WAVEFORMATEX** mix) {
    return client_->GetMixFormat(mix);
}

HRESULT AudioClientInitAdapter::initialize(AUDCLNT_SHAREMODE shareMode, DWORD streamFlags,
                                           REFERENCE_TIME bufferDuration,
                                           REFERENCE_TIME periodicity,
                                           const WAVEFORMATEX* format) {
    return client_->Initialize(shareMode, streamFlags, bufferDuration, periodicity,
                               format, nullptr);
}

Result streamInitShared(AudioClientInit& client, const StreamInitRequest& req,
                        StreamInitOutcome& out) {
    const REFERENCE_TIME dur = req.params.bufferMs
        ? static_cast<REFERENCE_TIME>(req.params.bufferMs) * 10'000
        : 10'000'000 / 10; // default: 100 ms buffer
    const DWORD extraFlags = req.extraFlags;

    if (req.requested) {
        out.actualFormat = *req.requested;
        out.frameBytes = out.actualFormat.blockAlign();
        WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(*req.requested);
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            extraFlags;
        HRESULT hr = client.initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0,
                                       reinterpret_cast<WAVEFORMATEX*>(&wfx));
        WA_LOG(wa::log::Level::Debug, "StreamInit", "Initialize(shared,requested)",
               "fmt=" + wa::formatAudio(*req.requested), wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Err, "StreamInit", "Initialize(shared,requested)",
                   "fmt=" + wa::formatAudio(*req.requested), wa::log::hrName(hr));
            return HrToResult(hr, "WasapiStream: Initialize(shared, requested)");
        }
        return Result::Ok();
    }

    WAVEFORMATEX* mix = nullptr;
    HRESULT hr = client.getMixFormat(&mix);
    WA_LOG(wa::log::Level::Debug, "StreamInit", "GetMixFormat", "", wa::log::hrName(hr));
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "StreamInit", "GetMixFormat", "", wa::log::hrName(hr));
        return HrToResult(hr, "WasapiStream: GetMixFormat");
    }
    if (!mix) return Result::Fail(-1, "WasapiStream: GetMixFormat returned null");
    out.actualFormat = fromWaveFormat(mix);
    out.frameBytes = out.actualFormat.blockAlign();
    hr = client.initialize(AUDCLNT_SHAREMODE_SHARED,
                           AUDCLNT_STREAMFLAGS_EVENTCALLBACK | extraFlags,
                           dur, 0, mix);
    WA_LOG(wa::log::Level::Debug, "StreamInit", "Initialize(shared)",
           "fmt=" + wa::formatAudio(out.actualFormat), wa::log::hrName(hr));
    CoTaskMemFree(mix);
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "StreamInit", "Initialize(shared)",
               "fmt=" + wa::formatAudio(out.actualFormat), wa::log::hrName(hr));
        return HrToResult(hr, "WasapiStream: Initialize(shared)");
    }
    return Result::Ok();
}

} // namespace wa
