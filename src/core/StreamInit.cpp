#include "StreamInit.h"
#include "FormatSpec.h"
#include "Log.h"
#include "AudioFormatStr.h"
#include <cstdio>
#include <string>
#include <vector>

namespace wa {
namespace {

DWORD sharedInitFlags(const StreamInitRequest& req) {
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | req.extraFlags;
    const bool addAutoConvert =
        req.params.autoConvert == AutoConvert::Force ||
        (req.params.autoConvert == AutoConvert::Default && req.requested != nullptr);
    if (addAutoConvert) {
        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                 AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }
    return flags;
}

std::string initArgs(const AudioFormat& fmt, DWORD flags) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%X", static_cast<unsigned>(flags));
    return "fmt=" + wa::formatAudio(fmt) + " flags=" + buf;
}

} // namespace

AudioClientInitAdapter::AudioClientInitAdapter(ComPtr<IAudioClient>& client, IMMDevice* device)
    : client_(client), device_(device) {}

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

HRESULT AudioClientInitAdapter::isFormatSupported(AUDCLNT_SHAREMODE shareMode,
                                                  const WAVEFORMATEX* format) {
    return client_->IsFormatSupported(shareMode, format, nullptr);
}

HRESULT AudioClientInitAdapter::getDevicePeriod(REFERENCE_TIME* defaultPeriod,
                                                REFERENCE_TIME* minimumPeriod) {
    return client_->GetDevicePeriod(defaultPeriod, minimumPeriod);
}

HRESULT AudioClientInitAdapter::getBufferSize(UINT32* frames) {
    return client_->GetBufferSize(frames);
}

HRESULT AudioClientInitAdapter::rebuild() {
    if (!device_) return E_POINTER;
    // Exclusive NOT_ALIGNED requires a fresh IAudioClient before retry.
    // Exclusive open rejects client properties, so re-Activate does not drop them.
    client_.Reset();
    return device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void**>(client_.GetAddressOf()));
}

HRESULT AudioClientInitAdapter::setClientProperties(const AudioClientProperties& props) {
    ComPtr<IAudioClient2> client2;
    HRESULT hr = client_->QueryInterface(__uuidof(IAudioClient2),
                     reinterpret_cast<void**>(client2.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "AudioClientInitAdapter", "QueryInterface(IAudioClient2)",
           "", wa::log::hrName(hr));
    if (FAILED(hr) || !client2.Get()) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = client2->SetClientProperties(&props);
    WA_LOG(wa::log::Level::Debug, "AudioClientInitAdapter", "SetClientProperties",
           "", wa::log::hrName(hr));
    return hr;
}

HRESULT AudioClientInitAdapter::isOffloadCapable(AUDIO_STREAM_CATEGORY category, BOOL* capable) {
    ComPtr<IAudioClient2> client2;
    HRESULT hr = client_->QueryInterface(__uuidof(IAudioClient2),
                     reinterpret_cast<void**>(client2.GetAddressOf()));
    WA_LOG(wa::log::Level::Debug, "AudioClientInitAdapter", "QueryInterface(IAudioClient2)",
           "", wa::log::hrName(hr));
    if (FAILED(hr) || !client2.Get()) return FAILED(hr) ? hr : E_NOINTERFACE;
    hr = client2->IsOffloadCapable(category, capable);
    WA_LOG(wa::log::Level::Debug, "AudioClientInitAdapter", "IsOffloadCapable",
           "", wa::log::hrName(hr));
    return hr;
}

Result streamInitShared(AudioClientInit& client, const StreamInitRequest& req,
                        StreamInitOutcome& out) {
    const REFERENCE_TIME dur = req.params.bufferMs
        ? static_cast<REFERENCE_TIME>(req.params.bufferMs) * 10'000
        : 10'000'000 / 10; // default: 100 ms buffer
    const DWORD flags = sharedInitFlags(req);

    if (req.requested) {
        out.actualFormat = *req.requested;
        out.frameBytes = out.actualFormat.blockAlign();
        WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(*req.requested);
        HRESULT hr = client.initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0,
                                       reinterpret_cast<WAVEFORMATEX*>(&wfx));
        WA_LOG(wa::log::Level::Debug, "StreamInit", "Initialize(shared,requested)",
               initArgs(*req.requested, flags), wa::log::hrName(hr));
        if (FAILED(hr)) {
            WA_LOG(wa::log::Level::Err, "StreamInit", "Initialize(shared,requested)",
                   initArgs(*req.requested, flags), wa::log::hrName(hr));
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
    hr = client.initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, mix);
    WA_LOG(wa::log::Level::Debug, "StreamInit", "Initialize(shared)",
           initArgs(out.actualFormat, flags), wa::log::hrName(hr));
    CoTaskMemFree(mix);
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "StreamInit", "Initialize(shared)",
               initArgs(out.actualFormat, flags), wa::log::hrName(hr));
        return HrToResult(hr, "WasapiStream: Initialize(shared)");
    }
    return Result::Ok();
}

Result streamInitExclusive(AudioClientInit& client, const StreamInitRequest& req,
                           StreamInitOutcome& out) {
    if (req.params.autoConvert == AutoConvert::Force) {
        WA_LOG(wa::log::Level::Debug, "StreamInit", "AutoConvert",
               "mode=exclusive value=Force", "rejected");
        return Result::Fail(-1, "WasapiStream: exclusive AutoConvert Force is not supported");
    }

    std::vector<AudioFormat> candidates;
    if (req.requested) {
        candidates.push_back(*req.requested);
    } else if (req.direction == StreamInitDirection::Capture) {
        candidates = defaultExclusiveCaptureCandidates();
    } else {
        return Result::Fail(-1, "WasapiStream: exclusive render requires an explicit format");
    }

    const int idx = selectSupportedFormat(candidates, [&](const AudioFormat& cand) {
        WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(cand);
        HRESULT probeHr = client.isFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                   reinterpret_cast<WAVEFORMATEX*>(&wfx));
        WA_LOG(wa::log::Level::Debug, "StreamInit", "IsFormatSupported(exclusive)",
               "fmt=" + wa::formatAudio(cand), wa::log::hrName(probeHr));
        return probeHr == S_OK;
    });
    if (idx < 0) {
        return Result::Fail(static_cast<long>(AUDCLNT_E_UNSUPPORTED_FORMAT),
                            "WasapiStream: no supported exclusive format");
    }

    out.actualFormat = candidates[static_cast<size_t>(idx)];
    out.frameBytes = out.actualFormat.blockAlign();

    REFERENCE_TIME defPer = 0, minPer = 0;
    HRESULT hrGP = client.getDevicePeriod(&defPer, &minPer);
    WA_LOG(wa::log::Level::Warn, "StreamInit", "GetDevicePeriod",
           "def=" + std::to_string(defPer) + " min=" + std::to_string(minPer),
           wa::log::hrName(hrGP));
    REFERENCE_TIME dur = req.params.bufferMs
        ? static_cast<REFERENCE_TIME>(req.params.bufferMs) * 10'000
        : minPer;

    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | req.extraFlags;
    WAVEFORMATEXTENSIBLE wfx = toWaveFormatExtensible(out.actualFormat);
    HRESULT hr = client.initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, dur, dur,
                                   reinterpret_cast<WAVEFORMATEX*>(&wfx));
    WA_LOG(wa::log::Level::Debug, "StreamInit", "Initialize(exclusive)",
           initArgs(out.actualFormat, flags), wa::log::hrName(hr));
    if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
        UINT32 aligned = 0;
        HRESULT hrGBS = client.getBufferSize(&aligned);
        WA_LOG(wa::log::Level::Warn, "StreamInit", "GetBufferSize(realign)",
               "frames=" + std::to_string(aligned), wa::log::hrName(hrGBS));
        dur = alignedBufferDuration100ns(out.actualFormat.sampleRate, aligned);
        HRESULT hr2 = client.rebuild();
        WA_LOG(wa::log::Level::Debug, "StreamInit", "Activate(realign)", "",
               wa::log::hrName(hr2));
        if (FAILED(hr2)) {
            WA_LOG(wa::log::Level::Err, "StreamInit", "Activate(realign)", "",
                   wa::log::hrName(hr2));
            return HrToResult(hr2, "WasapiStream: exclusive realign Activate");
        }
        WAVEFORMATEXTENSIBLE wfx2 = toWaveFormatExtensible(out.actualFormat);
        hr = client.initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, dur, dur,
                               reinterpret_cast<WAVEFORMATEX*>(&wfx2));
        WA_LOG(wa::log::Level::Debug, "StreamInit", "Initialize(exclusive,realign)",
               initArgs(out.actualFormat, flags), wa::log::hrName(hr));
    }
    if (FAILED(hr)) {
        WA_LOG(wa::log::Level::Err, "StreamInit", "Initialize(exclusive)",
               initArgs(out.actualFormat, flags), wa::log::hrName(hr));
        return HrToResult(hr, "WasapiStream: Initialize(exclusive)");
    }
    return Result::Ok();
}

Result streamInit(AUDCLNT_SHAREMODE shareMode, AudioClientInit& client,
                  const StreamInitRequest& req, StreamInitOutcome& out) {
    if (shareMode == AUDCLNT_SHAREMODE_EXCLUSIVE)
        return streamInitExclusive(client, req, out);
    return streamInitShared(client, req, out);
}

} // namespace wa
