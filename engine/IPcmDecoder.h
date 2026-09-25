#pragma once
#include <cstdint>
#include <string>

// ─────────────────────────────────────────────────────────
// IPcmDecoder
//
// 各フォーマット固有のデコーダー(Flac/Wav/Aiff/...)が実装する共通
// インターフェース。PcmDualEngineはこれを介して「interleaved
// int32(L,R,...、左詰め正規化済み)をフレーム単位で取り出す」ことだけを
// 知っていればよく、フォーマットごとの違いを一切意識しない。
//
// ★ 「左詰め正規化」とは：例えば16bitサンプルなら値を<<16し、32bit
// フルスケールで表現すること。dr_flacのdrflac_read_pcm_frames_s32が
// この規約で出力するため、他のデコーダーも合わせる。
// ─────────────────────────────────────────────────────────
class IPcmDecoder {
public:
    virtual ~IPcmDecoder() = default;

    virtual bool Open(const std::wstring& filePath) = 0;
    virtual void Close() = 0;

    virtual uint32_t GetSampleRate() const = 0;
    virtual uint32_t GetChannels() const = 0;
    virtual uint32_t GetBitsPerSample() const = 0;  // 表示用（実際の出力は常にint32）
    virtual uint64_t GetTotalFrames() const = 0;

    // interleaved int32(L,R,...)で最大frameCount分読み込み、実際に読めた
    // フレーム数を返す。0はEOF。
    virtual uint64_t ReadFrames(int32_t* out, uint64_t frameCount) = 0;

    virtual bool SeekToFrame(uint64_t frame) = 0;
};
