#pragma once
#include "IPcmDecoder.h"
#include <vector>

struct stb_vorbis;

// ─────────────────────────────────────────────────────────
// OggVorbisPcmDecoder
//
// stb_vorbis（パブリックドメインの単一ファイル実装。engine/stb_vorbis.c）を
// 使った Ogg Vorbis (.ogg) のデコーダー。IPcmDecoderの実装として
// PcmDualEngineから利用される（dr_flacと同じく外部ライブラリ不要）。
//
// ★ 出力
//   Vorbisは非可逆でデコーダー出力はfloat。精度を落とさないようfloatで
//   受け取り、doubleで計算・クリップしてleft-justified int32へ変換する。
//   GetBitsPerSample()は0（MP3/AACと同じ扱い）。
//
// ★ チャンネル
//   モノラルは同じ値をL/Rへ複製。3ch以上はVorbisのチャンネル順
//   （FL,FC,FR,…）の都合で単純に先頭2chを取ると誤るため、Open()で
//   失敗させ、呼び出し側のフォールバックに任せる。
//
// ★ シーク
//   stb_vorbis_seek()はサンプル単位で正確。
// ─────────────────────────────────────────────────────────
class OggVorbisPcmDecoder : public IPcmDecoder {
public:
    OggVorbisPcmDecoder() = default;
    ~OggVorbisPcmDecoder() override { Close(); }

    bool Open(const std::wstring& filePath) override;
    void Close() override;

    uint32_t GetSampleRate()    const override { return m_sampleRate; }
    uint32_t GetChannels()      const override { return 2; } // 常にステレオで出力
    uint32_t GetBitsPerSample() const override { return 0; }
    uint64_t GetTotalFrames()   const override { return m_totalFrames; }

    uint64_t ReadFrames(int32_t* out, uint64_t frameCount) override;
    bool SeekToFrame(uint64_t frame) override;

private:
    stb_vorbis* m_vorbis = nullptr;
    uint32_t m_sampleRate  = 0;
    uint32_t m_srcChannels = 0;
    uint64_t m_totalFrames = 0;
    std::vector<float> m_floatBuf;
};
