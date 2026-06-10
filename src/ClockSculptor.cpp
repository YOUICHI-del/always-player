#include "ClockSculptor.h"

ClockSculptor::ClockSculptor(double a, double b)
    : alpha(a), beta(b), driftState(0.0) {}

double ClockSculptor::sculpt(int64_t deltaUs) {
    double jitter = static_cast<double>(deltaUs);
    driftState = driftState * 0.999 + jitter * 0.001;
    double C = alpha * jitter + beta * driftState;
    return C;
}
