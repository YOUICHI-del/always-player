#pragma once
#include <cstdint>

class TimerMonitor {
public:
    TimerMonitor();
    uint64_t now();  // マイクロ秒単位の高精度タイムスタンプ
private:
    double freqInv;
};
