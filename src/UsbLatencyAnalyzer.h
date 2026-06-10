#pragma once
#include <cstdint>

class UsbLatencyAnalyzer {
public:
    UsbLatencyAnalyzer(uint64_t idealPeriodUs);

    // パケット到着時刻を渡すと Δt を返す
    int64_t processTimestamp(uint64_t timestampUs);

private:
    uint64_t idealPeriod;
    uint64_t expectedNext;
    bool initialized;
};
