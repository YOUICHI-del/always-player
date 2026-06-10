#include "TimerMonitor.h"
#include <windows.h>

TimerMonitor::TimerMonitor() {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    freqInv = 1.0 / static_cast<double>(freq.QuadPart);
}

uint64_t TimerMonitor::now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    double micro = static_cast<double>(t.QuadPart) * freqInv * 1'000'000.0;
    return static_cast<uint64_t>(micro);
}
