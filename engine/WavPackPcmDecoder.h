#pragma once
#include "IPcmDecoder.h"

struct WavpackContext;

// ─────────────────────────────────────────────────────────
// WavPackPcmDecoder
//
// libwavpack（デコード専用サブセット。engine/wavpack/配下）を使った
// WavPack(.wv)ファイルのデコーダー。IPcmDecoderの実装として
// PcmDualEngineから利用される。
//
// libwavpackのWavpackUnpackSamples()は「right-justified」（右詰め）で
// サンプルを返す仕様のため（例：16bitなら±32k程度の値）、本クラスの
// ReadFrames()内で (32 - bitsPerSample) 分だけ左シフトし、
// dr_flac等と同じ「left-justified int32」規約に変換して出力する。
// ─────────────────────────────────────────────────────────
class WavPackPcmDecoder : public IPcmDecoder {
public:
    WavPackPcmDecoder() = default;
    ~WavPackPcmDecoder() override { Close(); }

    bool Open(const std::wstring& filePath) override;
    void Close() override;

    uint32_t GetSampleRate()    const override { return m_sampleRate; }
    uint32_t GetChannels()      const override { return m_channels; }
    uint32_t GetBitsPerSample() const override { return m_bitsPerSample; }
    uint64_t GetTotalFrames()   const override { return m_totalFrames; }

    uint64_t ReadFrames(int32_t* out, uint64_t frameCount) override;
    bool SeekToFrame(uint64_t frame) override;

private:
    WavpackContext *m_ctx = nullptr;
    uint32_t m_sampleRate    = 0;
    uint32_t m_channels      = 0;
    uint32_t m_bitsPerSample = 0;
    uint64_t m_totalFrames   = 0;
    bool     m_isFloat       = false;
    int      m_shiftAmount   = 0; // 32 - bitsPerSample（整数モードのみ使用）
};
