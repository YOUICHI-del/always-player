#include "AlphaStyleInterpolator.h"
#include <cstdio>
#include <vector>

int main()
{
    const int N = 2000;  // taps(192)の半分(96)より十分大きい余白を確保
    std::vector<float> inL(N, 0.0f), inR(N, 0.0f);
    inL[N/2] = 1.0f;
    inR[N/2] = 1.0f;

    AlphaStyleInterpolator interp(192, 120.0);
    std::vector<float> outL, outR;
    interp.process(inL.data(), inR.data(), N, outL, outR);

    for (float v : outL) std::printf("%f\n", v);
    return 0;
}
