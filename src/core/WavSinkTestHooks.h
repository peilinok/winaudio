#pragma once

// This header and its calls are compiled only into the dedicated test target.
#ifdef WA_WAV_SINK_TESTING
namespace wa {
class WavSink;
namespace wavSinkTest {
enum class Point { AfterSnapshot, AfterAdmission, BeforePublish };
enum class Preparation { None, Ring, Scratch, Thread };
void checkpoint(Point point, const WavSink* sink);
void drainObserved(const WavSink* sink, bool active);
void prepare(Preparation operation);
} // namespace wavSinkTest
} // namespace wa
#endif
