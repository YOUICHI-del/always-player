#pragma once
#include "IPcmDecoder.h"
#include <vector>

struct OggOpusFile;

// ─────────────────────────────────────────────────────────
// OpusPcmDecoder
//
// libopusfile（libopus＋libogg。vcpkgの静的ライブラリ）を使った
// Ogg Opus (.opus / Opus入りの.ogg) のデコーダー。IPcmDecoderの実装として
// PcmDualEngineから利用される。
//
// ★ Opusの仕様上、デコード結果は常に48kHz。
// ★ op_read_float_stereo() を使うので、モノラルは両chへ、5.1ch等は
//   Opus規格どおりの係数でステレオへダウンミックスされた値が得られる。
// ★ 長さ・シークはプリスキップ（エンコーダー遅延）を除いた正確な
//   サンプル位置で扱われるため、Opusのアルバムはそのままギャップレスになる。
// ★ 出力はfloat → doubleで計算・クリップしてleft-justified int32へ。
//   GetBitsPerSample()は0（MP3/AACと同じ扱い）。
// ─────────────────────────────────────────────────────────
class OpusPcmDecoder : public IPcmDecoder {
public:
    OpusPcmDecoder() = default;
    ~OpusPcmDecoder() override { Close(); }

    bool Open(const std::wstring& filePath) override;
    void Close() override;

    uint32_t GetSampleRate()    const override { return 48000; }
    uint32_t GetChannels()      const override { return 2; }
    uint32_t GetBitsPerSample() const override { return 0; }
    uint64_t GetTotalFrames()   const override { return m_totalFrames; }

    uint64_t ReadFrames(int32_t* out, uint64_t frameCount) override;
    bool SeekToFrame(uint64_t frame) override;

private:
    OggOpusFile* m_of = nullptr;
    uint64_t m_totalFrames = 0;
    std::vector<float> m_floatBuf;
};
