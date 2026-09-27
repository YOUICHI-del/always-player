#pragma once
#include "IPcmDecoder.h"
#include <vector>

struct IMFSourceReader;

// ─────────────────────────────────────────────────────────
// MfPcmDecoder
//
// Windows Media Foundation (IMFSourceReader) を使った MP3 / AAC / M4A の
// デコーダー。IPcmDecoderの実装としてPcmDualEngineから利用される。
// OS標準のデコーダーを使うため、特許ライセンスを自前で抱えずに済む。
//
// ★ 出力形式の選び方
//   ・MP3 / AAC（非可逆）：デコーダー本来の精度を落とさないよう、
//     MFから32bit floatで受け取り、left-justified int32へ変換する。
//     GetBitsPerSample()は0を返す（元々「ビット深度」を持たない形式のため。
//     従来のmpv経路でTagLibが返していた値と同じ扱い）。
//   ・ALAC（M4Aの可逆）/ PCM：元のビット深度の整数PCMで受け取り、
//     そのまま左詰めする（ビットパーフェクト）。
//
// ★ チャンネル
//   PcmDualEngineはステレオ前提のため、モノラルは同じ値をL/Rへ複製
//   （値は一切変えない）、3ch以上はMFのデコーダーにステレオへの
//   ダウンミックスを要求する。
//
// ★ シーク
//   MFのSetCurrentPosition()はフレーム境界単位の近似シークなので、
//   シーク後最初のサンプルのタイムスタンプから目標位置までの差分を
//   読み捨てて、サンプル単位で正確な位置に合わせる。
// ─────────────────────────────────────────────────────────
class MfPcmDecoder : public IPcmDecoder {
public:
    MfPcmDecoder() = default;
    ~MfPcmDecoder() override { Close(); }

    bool Open(const std::wstring& filePath) override;
    void Close() override;

    uint32_t GetSampleRate()    const override { return m_sampleRate; }
    uint32_t GetChannels()      const override { return 2; } // 常にステレオで出力
    uint32_t GetBitsPerSample() const override { return m_bitsPerSample; }
    uint64_t GetTotalFrames()   const override { return m_totalFrames; }

    uint64_t ReadFrames(int32_t* out, uint64_t frameCount) override;
    bool SeekToFrame(uint64_t frame) override;

private:
    enum class SampleKind { Float32, Int16, Int24, Int32 };

    // MFから次のサンプル（圧縮フレーム1つ分程度）を読み、m_pendingへ
    // ステレオint32で追記する。EOFならfalse。
    bool FillPending();

    IMFSourceReader* m_reader = nullptr;
    bool     m_mfStarted     = false;

    uint32_t m_sampleRate    = 0;
    uint32_t m_srcChannels   = 0;   // MFから受け取るチャンネル数(1 or 2)
    uint32_t m_bitsPerSample = 0;   // 表示用（非可逆は0）
    uint64_t m_totalFrames   = 0;
    SampleKind m_kind        = SampleKind::Float32;

    std::vector<int32_t> m_pending;  // interleaved stereo int32
    size_t   m_pendingPos    = 0;    // m_pending内の読み出し位置（サンプル単位）

    // シーク後、最初のサンプルでこのフレーム位置まで読み捨てる。-1なら無効。
    int64_t  m_seekTargetFrame = -1;
    bool     m_eof           = false;
};
