#pragma once
#include <cstdint>
class AppUi;

// Only linked into the explicitly built performance executable.
class PerformanceFixture {
public:
    static bool start(AppUi& ui);
    static bool advance(AppUi& ui);
    static void scrollCharts(AppUi& ui);
    static void chartPosition(uint64_t source, bool spectrum);
};
