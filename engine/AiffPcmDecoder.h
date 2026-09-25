#pragma once
#include "IPcmDecoder.h"
#include <fstream>

// AIFF/AIFC(非圧縮'NONE'のみ)の最小限のパーサー。
// ビッグエンディアンの整数PCM(8/16/24/32bit)に対応し、常にinterleaved
// int32(左詰め正規化済み)へ変換して返す。
// ★ 他の新エンジン系コンポーネントがステレオ固定のため、2ch以外の
//   ファイルはOpen()がfalseを返す。
class AiffPcmDecoder : public IPcmDecoder {
public:
    bool Open(const std::wstring& filePath) override;
    void Close() override;

    uint32_t GetSampleRate()    const override { return m_sampleRate; }
    uint32_t GetChannels()      const override { return m_channels; }
    uint32_t GetBitsPerSample() const override { return m_bitsPerSample; }
    uint64_t GetTotalFrames()   const override { return m_totalFrames; }

    uint64_t ReadFrames(int32_t* out, uint64_t frameCount) override;
    bool SeekToFrame(uint64_t frame) override;

private:
    std::ifstream m_file;
    uint32_t m_sampleRate = 0;
    uint32_t m_channels = 0;
    uint32_t m_bitsPerSample = 0;
    uint32_t m_bytesPerSample = 0;   // ceil(bitsPerSample/8)
    uint32_t m_bytesPerFrame = 0;
    uint64_t m_totalFrames = 0;
    uint64_t m_dataStartPos = 0;
    uint64_t m_framesReadSoFar = 0;
};
