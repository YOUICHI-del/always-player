#pragma once
#include <cstdint>

class ClockSculptor {
public:
    ClockSculptor(double alpha, double beta);

    // Δt を渡すと補正量 C[n] を返す
    double sculpt(int64_t deltaUs);

private:
    double alpha;  // jitter smoothing
    double beta;   // drift correction

    double driftState;
};