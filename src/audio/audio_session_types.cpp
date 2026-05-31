#include "audio_session_types.h"

namespace winaudio {

void NullSessionEventSink::OnLogLine(const std::wstring& line) {
  (void)line;
}

void NullSessionEventSink::OnStatsUpdated(const SessionRuntimeStats& stats) {
  (void)stats;
}

void NullSessionEventSink::OnWaveformUpdated(
    AudioDirection direction,
    const std::vector<WaveformEnvelopePoint>& waveform,
    const MeterValues& meter) {
  (void)direction;
  (void)waveform;
  (void)meter;
}

void NullSessionEventSink::OnDevicesUpdated(
    const DeviceEnumerationSnapshot& snapshot) {
  (void)snapshot;
}

void NullSessionEventSink::OnSessionStateChanged(const std::wstring& state) {
  (void)state;
}

}  // namespace winaudio
