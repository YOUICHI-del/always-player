#include "AlphaStyleInterpolator.h"
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>

#pragma pack(push, 1)
struct WavHeader {
    char riff[4]; uint32_t fileSize; char wave[4];
    char fmt[4]; uint32_t fmtSize;
    uint16_t audioFormat; uint16_t numChannels;
    uint32_t sampleRate; uint32_t byteRate;
    uint16_t blockAlign; uint16_t bitsPerSample;
};
#pragma pack(pop)

bool ReadWav(const char* path, std::vector<float>& L, std::vector<float>& R, uint32_t& sampleRate)
{
    FILE* f = fopen(path, "rb");
    if (!f) { printf("cannot open input: %s\n", path); return false; }

    char chunkId[4]; uint32_t chunkSize;
    fread(chunkId, 1, 4, f); fread(&chunkSize, 4, 1, f);
    if (memcmp(chunkId, "RIFF", 4) != 0) { printf("not RIFF\n"); fclose(f); return false; }
    char wave[4]; fread(wave, 1, 4, f);

    uint16_t bitsPerSample = 0, numChannels = 0;
    long dataPos = 0; uint32_t dataSize = 0;

    while (fread(chunkId, 1, 4, f) == 4) {
        fread(&chunkSize, 4, 1, f);
        if (memcmp(chunkId, "fmt ", 4) == 0) {
            uint16_t audioFormat; fread(&audioFormat, 2, 1, f);
            fread(&numChannels, 2, 1, f);
            fread(&sampleRate, 4, 1, f);
            uint32_t byteRate; fread(&byteRate, 4, 1, f);
            uint16_t blockAlign; fread(&blockAlign, 2, 1, f);
            fread(&bitsPerSample, 2, 1, f);
            if (chunkSize > 16) fseek(f, chunkSize - 16, SEEK_CUR);
        } else if (memcmp(chunkId, "data", 4) == 0) {
            dataPos = ftell(f);
            dataSize = chunkSize;
            break;
        } else {
            fseek(f, chunkSize, SEEK_CUR);
        }
    }
    if (dataPos == 0) { printf("no data chunk\n"); fclose(f); return false; }

    fseek(f, dataPos, SEEK_SET);
    int bytesPerSample = bitsPerSample / 8;
    size_t totalSamples = dataSize / bytesPerSample;
    size_t frames = totalSamples / numChannels;

    std::vector<uint8_t> raw(dataSize);
    fread(raw.data(), 1, dataSize, f);
    fclose(f);

    L.resize(frames); R.resize(frames);
    for (size_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < (int)numChannels && ch < 2; ++ch) {
            size_t offset = (i * numChannels + ch) * bytesPerSample;
            int32_t v = 0;
            if (bitsPerSample == 16) {
                int16_t s; memcpy(&s, &raw[offset], 2); v = s;
                float f32 = v / 32768.0f;
                if (ch == 0) L[i] = f32; else R[i] = f32;
            } else if (bitsPerSample == 24) {
                int32_t s = (raw[offset]) | (raw[offset+1] << 8) | (raw[offset+2] << 16);
                if (s & 0x800000) s |= 0xFF000000;
                float f32 = s / 8388608.0f;
                if (ch == 0) L[i] = f32; else R[i] = f32;
            } else if (bitsPerSample == 32) {
                int32_t s; memcpy(&s, &raw[offset], 4);
                float f32 = s / 2147483648.0f;
                if (ch == 0) L[i] = f32; else R[i] = f32;
            }
        }
        if (numChannels == 1) R[i] = L[i];
    }
    return true;
}

void WriteWav24(const char* path, const std::vector<float>& L, const std::vector<float>& R, uint32_t sampleRate)
{
    FILE* f = fopen(path, "wb");
    size_t frames = L.size();
    uint32_t dataSize = (uint32_t)(frames * 2 * 3); // 24bit stereo

    WavHeader h{};
    memcpy(h.riff, "RIFF", 4);
    memcpy(h.wave, "WAVE", 4);
    memcpy(h.fmt, "fmt ", 4);
    h.fmtSize = 16;
    h.audioFormat = 1;
    h.numChannels = 2;
    h.sampleRate = sampleRate;
    h.bitsPerSample = 24;
    h.blockAlign = h.numChannels * h.bitsPerSample / 8;
    h.byteRate = h.sampleRate * h.blockAlign;
    h.fileSize = 36 + dataSize;

    fwrite(&h, sizeof(h), 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);

    for (size_t i = 0; i < frames; ++i) {
        for (int ch = 0; ch < 2; ++ch) {
            float v = ch == 0 ? L[i] : R[i];
            if (v > 1.0f) v = 1.0f;
            if (v < -1.0f) v = -1.0f;
            int32_t s = (int32_t)(v * 8388607.0f);
            uint8_t b0 = s & 0xFF, b1 = (s >> 8) & 0xFF, b2 = (s >> 16) & 0xFF;
            fwrite(&b0, 1, 1, f); fwrite(&b1, 1, 1, f); fwrite(&b2, 1, 1, f);
        }
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    if (argc < 3) {
        printf("usage: alphastyle_offline.exe input.wav output.wav\n");
        return 1;
    }
    std::vector<float> L, R;
    uint32_t sampleRate = 0;
    if (!ReadWav(argv[1], L, R, sampleRate)) return 1;
    printf("input: %zu frames @ %u Hz\n", L.size(), sampleRate);

    if (sampleRate != 44100) {
        printf("warning: input is not 44100Hz, result may be wrong (got %u)\n", sampleRate);
    }

    AlphaStyleInterpolator interp(192, 120.0);
    std::vector<float> outL, outR;
    interp.process(L.data(), R.data(), (int)L.size(), outL, outR);

    uint32_t outRate = sampleRate * 8;
    WriteWav24(argv[2], outL, outR, outRate);
    printf("output: %zu frames @ %u Hz -> %s\n", outL.size(), outRate, argv[2]);
    return 0;
}
