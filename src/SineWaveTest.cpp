// SineWaveTest.cpp
//
// Standalone test for WasapiExclusiveOutput.
// Generates a sine wave on the spot and writes it directly to WriteFrames(),
// bypassing FlacDecoder / FdChannelThread entirely. Purpose: verify that
// WASAPI exclusive-mode initialization and buffer writes work correctly.
//
#include "WasapiExclusiveOutput.h"

#include <cmath>
#include <cstdio>
#include <vector>
#include <thread>
#include <chrono>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kTestFrequencyHz = 440.0; // A4
    constexpr int    kTestDurationSec = 5;
}

// 24-bit samples are packed into the upper 24 bits of a 32-bit container,
// which is the common convention for WASAPI exclusive mode.
static void FillSineBlock(std::vector<int32_t>& buf, uint32_t frameCount,
                           uint32_t channels, uint32_t sampleRate,
                           double& phase)
{
    buf.resize(static_cast<size_t>(frameCount) * channels);
    const double phaseStep = 2.0 * kPi * kTestFrequencyHz / sampleRate;

    for (uint32_t f = 0; f < frameCount; ++f) {
        // Keep the volume low (-20dBFS-ish) to protect ears and speakers.
        const double amplitude = 0.1;
        const double sampleValue = std::sin(phase) * amplitude;

        // Convert [-1.0, 1.0] to a 24-bit integer, placed in the upper 24 bits
        // of a 32-bit container.
        const int32_t sample24 = static_cast<int32_t>(sampleValue * 8388607.0); // 2^23 - 1
        const int32_t sampleInContainer = sample24 << 8;

        for (uint32_t ch = 0; ch < channels; ++ch) {
            buf[static_cast<size_t>(f) * channels + ch] = sampleInContainer;
        }

        phase += phaseStep;
        if (phase > 2.0 * kPi) phase -= 2.0 * kPi;
    }
}

int main()
{
    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
        std::printf("CoInitializeEx failed: 0x%08lx\n", hrCo);
        return 1;
    }

    // IMPORTANT: WasapiExclusiveOutput holds COM interface pointers (IAudioClient,
    // IAudioRenderClient, etc). Those must be released (their destructor must run)
    // BEFORE CoUninitialize() is called below. Putting "output" in its own scope
    // guarantees its destructor runs at the closing brace, which is before
    // CoUninitialize() executes. Without this scope, "output" (declared as a plain
    // local variable) would be destroyed only when main() returns -- i.e. AFTER
    // CoUninitialize() already tore down the COM apartment, causing an access
    // violation inside Release().
    int resultCode = 0;
    {
        WasapiExclusiveOutput output;

        AudioFormatSpec format;
        format.sampleRate    = 44100;
        format.bitsPerSample = 24;
        format.channels      = 2;

        std::printf("Initializing WASAPI exclusive: %u Hz / %u bit / %u ch ...\n",
                    format.sampleRate, format.bitsPerSample, format.channels);

        if (!output.Initialize(format)) {
            std::printf("Initialize() failed. Is the output device busy or format unsupported?\n");
            resultCode = 1;
        } else {
            const uint32_t actualFrames = output.GetActualBufferFrames();
            const AudioFormatSpec actualFormat = output.GetActualFormat();
            std::printf("Initialized OK. Actual buffer frames = %u (this replaces any hardcoded 9600/441 constant)\n",
                        actualFrames);
            std::printf("Actual format: %u Hz / %u bit / %u ch\n",
                        actualFormat.sampleRate, actualFormat.bitsPerSample, actualFormat.channels);

            if (!output.Start()) {
                std::printf("Start() failed.\n");
                resultCode = 1;
            } else {
                std::printf("Playing %d seconds of 440Hz sine wave at low volume...\n", kTestDurationSec);

                double phase = 0.0;
                std::vector<int32_t> buffer;

                const auto startTime = std::chrono::steady_clock::now();
                while (std::chrono::duration_cast<std::chrono::seconds>(
                           std::chrono::steady_clock::now() - startTime).count() < kTestDurationSec) {

                    FillSineBlock(buffer, actualFrames, actualFormat.channels, actualFormat.sampleRate, phase);

                    const uint32_t written = output.WriteFrames(buffer.data(), actualFrames);
                    if (written == 0) {
                        std::printf("WriteFrames() returned 0 - underrun or device error.\n");
                        break;
                    }
                }

                output.Stop();
                std::printf("Done.\n");
            }
        }
    } // <-- output's destructor (COM Release calls) runs here, while COM is still valid

    CoUninitialize();
    return resultCode;
}
