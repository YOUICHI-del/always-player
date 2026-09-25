#pragma once
#include "IPcmDecoder.h"

// ★ 実装マクロ(DR_FLAC_IMPLEMENTATION)はここでは定義しない。
//   ヘッダは複数の.cppから読み込まれる可能性があるため、定義すると
//   「複数回定義」リンクエラーになる。実装はFlacPcmDecoder.cppの
//   先頭で一度だけ定義する。
#include "dr_flac.h"

class FlacPcmDecoder : public IPcmDecoder {
public:
    FlacPcmDecoder();
    ~FlacPcmDecoder() override;

    bool Open(const std::wstring& filePath) override;
    void Close() override;

    uint32_t GetSampleRate()    const override { return m_sampleRate; }
    uint32_t GetChannels()      const override { return m_channels; }
    uint32_t GetBitsPerSample() const override { return m_bitsPerSample; }
    uint64_t GetTotalFrames()   const override { return m_totalFrames; }

    uint64_t ReadFrames(int32_t* out, uint64_t frameCount) override;
    bool SeekToFrame(uint64_t frame) override;

private:
    drflac* m_flac = nullptr;
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    uint32_t m_bitsPerSample = 0;
    uint64_t m_totalFrames = 0;
};
