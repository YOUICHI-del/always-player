#pragma once
#include "IPcmDecoder.h"
#include <fstream>

// RIFF/WAVEの最小限のパーサー。PCM(1)とIEEE float(3)、および
// WAVE_FORMAT_EXTENSIBLE経由のそれらのサブフォーマットに対応。
// 8/16/24/32bit整数、32bit floatをサポートし、常にinterleaved
// int32(左詰め正規化済み)へ変換して返す。
// ★ 他の新エンジン系コンポーネント(WasapiExclusiveOutput等)がステレオ
//   固定のため、2ch以外のファイルはOpen()がfalseを返す。
class WavPcmDecoder : public IPcmDecoder {
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
    uint32_t m_bytesPerFrame = 0;
    uint64_t m_totalFrames = 0;
    uint64_t m_dataStartPos = 0;
    uint64_t m_dataSize = 0;
    uint64_t m_framesReadSoFar = 0;
    bool m_isFloat = false;
};
