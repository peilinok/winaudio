#pragma once
#include <cstdint>
#include <memory>
#include "WavSinkOutput.h"
class AppUi;
namespace wa { class CallLogWriter; }

// Only linked into the explicitly built performance executable.
class PerformanceFixture {
public:
    static std::shared_ptr<wa::CallLogWriter> exportWriter();
    static wa::WavSinkOutputFactory monitorRenderOutput();
    static bool monitorDump();
    static bool start(AppUi& ui);
    static bool advance(AppUi& ui);
    static void scrollCharts(AppUi& ui);
    static void chartPosition(uint64_t source, bool spectrum);
    static bool inspector();
    static bool liveInspector();
    static void prepareInspector(AppUi& ui);
    static bool filterEdited();
    static void scrollCalls();
    static void scrollParameters();
    static void inspectorSubmitted(AppUi& ui);
private:
    static bool startInspector(AppUi& ui);
};
