#include "HarmonicUpsampleDsp.h"
#include <cmath>

HarmonicUpsampleDsp::HarmonicUpsampleDsp(float harmonicAmount)
    : m_harmonicAmount(harmonicAmount) {
}

void HarmonicUpsampleDsp::Process(float* interleavedSamples, size_t frameCount) {
    const float wet = m_harmonicAmount;
    const float dry = 1.0f - wet;

    for (size_t i = 0; i < frameCount * 2; ++i) {
        float x = interleavedSamples[i];
        float saturated = std::tanh(x * 3.0f) / std::tanh(3.0f);
        interleavedSamples[i] = dry * x + wet * saturated;
    }
}
