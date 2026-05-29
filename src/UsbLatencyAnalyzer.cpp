#include "UsbLatencyAnalyzer.h"

UsbLatencyAnalyzer::UsbLatencyAnalyzer(uint64_t idealPeriodUs)
    : idealPeriod(idealPeriodUs), initialized(false) {}

int64_t UsbLatencyAnalyzer::processTimestamp(uint64_t ts) {
    if (!initialized) {
        expectedNext = ts + idealPeriod;
        initialized = true;
        return 0;
    }

    int64_t delta = static_cast<int64_t>(ts) - static_cast<int64_t>(expectedNext);
    expectedNext += idealPeriod;
    return delta;
}
