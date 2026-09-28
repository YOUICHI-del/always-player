#pragma once
#include "RingBuffer.h"
#include "IAudioOutputBackend.h"
#include "IPcmDecoder.h"
#include "SincResampler.h"
#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include <mutex>

constexpr size_t kPcmDecodeFrameSize = 4096;

// ─────────────────────────────────────────────────────────
// PcmDualEngine
//
// FlacDualEngineをデコーダー非依存に一般化したもの。拡張子に応じて
// IPcmDecoder実装(Flac/Wav/Aiff/WavPack/...)を選び、以降はフォーマットの
// 違いを一切意識せずデコード→(必要なら高品質リサンプル)→リングバッファ→
// WASAPI排他出力というパイプラインを提供する。
//
// ★ アップサンプリング(dsd8/hires4)：SetTargetSampleRate()で目標レートを
//   指定すると、デコードスレッド内でSincResampler(Kaiser窓sinc補間)に
//   よりネイティブレートから目標レートへ変換してからリングバッファへ
//   書き込む。指定しない（またはネイティブと同じレートを指定）場合は
//   従来通りの素通し（リサンプルなし）。
//
// ★ ギャップレス再生：PrepareGaplessNext()で次曲のデコーダーを事前に
//   オープンしておき、ConfirmGaplessTarget()で目標レート（＝現在の
//   GetSampleRate()と一致する場合のみ意味がある）を確定させておくと、
//   デコードスレッドが現在曲のEOFに達した瞬間、WASAPIストリーム／
//   リングバッファを一切止めずに次曲のデコードへシームレスに切り替わる。
//   出力レートが変わらない場合のみ意味を持つ仕組みであり、レート不一致の
//   判定は呼び出し側(Player::computeNewEngineTargetRate等)の責務。
// ─────────────────────────────────────────────────────────
class PcmDualEngine {
public:
    PcmDualEngine();
    ~PcmDualEngine();

    // ギャップレス：次曲プリロードのオープン結果、および遷移後のトラック情報。
    struct GaplessInfo {
        bool     ok               = false;
        uint32_t nativeSampleRate = 0;
        uint32_t bitsPerSample    = 0;
        uint64_t totalFrames      = 0;
        // ★ 遷移が起きた瞬間、リングバッファにまだ残っていた「前の曲の未再生分」
        //   （出力レート基準のフレーム数）。デコードはリングバッファの分だけ
        //   実再生より先行して進むため、デコードスレッドが次曲へ切り替わった
        //   瞬間＝実際にWASAPIから前の曲の音が鳴り終わった瞬間ではない。
        //   呼び出し側（Player）は、この分の再生が実際に終わるまで
        //   （backlogOutputFrames / 出力レート秒）UI状態の更新を遅らせること。
        uint64_t backlogOutputFrames = 0;
    };

    // extLowerは拡張子の小文字表記（ドット無し。例:"wav","flac","aiff"）。
    // 対応していない拡張子や、開けなかった場合はfalseを返す。
    bool Open(const std::wstring& filePath, const std::string& extLower);
    void Close();

    // ★ v10: 次のOpen()でDSF/DFFをDoPの形で開くか（それ以外の形式には影響しない）。
    void SetDopMode(bool on) { m_dopMode = on; }
    // 今開いている曲がDoPで出力中か
    bool IsDop() const { return m_isDop; }

    void AttachOutputBackend(IAudioOutputBackend* backend);

    // ★ アップサンプリング先のレートを指定する。Open()成功後、
    //   StartDecoding()より前に呼ぶこと。0、またはGetNativeSampleRate()と
    //   同じ値を渡すとリサンプルなし（ネイティブレートのまま）になる。
    //   呼び出し以降、GetSampleRate()はこの目標レートを返すようになる
    //   （WASAPI初期化・DspChain::Prepare()等はこちらを使う）。
    void SetTargetSampleRate(uint32_t targetRate);

    void StartDecoding();
    void StopDecoding();

    // シーク：デコードスレッドを一旦止め、リングバッファ・リサンプラーの
    // 内部状態をクリアしてから目的のPCMフレーム位置（ネイティブレート基準）
    // へジャンプする。呼び出し前にデコード中だった場合は、シーク後に
    // 自動的にデコードスレッドを再開する。
    bool Seek(double seconds);

    AudioRingBuffer* GetRingBuffer() { return m_ring.get(); }

    // 出力レート（リサンプル先。SetTargetSampleRate()未指定ならネイティブと同値）
    uint32_t GetSampleRate()       const { return m_outputSampleRate; }
    // デコーダーのネイティブ（原音）レート。シーク・長さ計算に使う。
    uint32_t GetNativeSampleRate() const { return m_nativeSampleRate; }
    uint32_t GetTotalChannels()    const { return m_channels; }
    uint32_t GetBitsPerSample()    const { return m_bitsPerSample; }
    // ネイティブ（原音）基準の総フレーム数。長さ計算に使う。
    uint64_t GetTotalFrames()      const { return m_totalFrames; }
    bool IsEndOfStream() const { return m_endOfStream.load(); }

    // ★ ギャップレス：次曲を事前にオープンする（現在の再生には影響しない）。
    //   戻り値のnativeSampleRateを使って、呼び出し側がcomputeNewEngineTargetRate()
    //   相当のロジックで目標レートを決定し、それが現在のGetSampleRate()と
    //   一致する場合のみConfirmGaplessTarget()を呼ぶこと。一致しない場合は
    //   CancelGapless()で破棄し、従来通りの（途切れを伴う）再オープンに任せる。
    GaplessInfo PrepareGaplessNext(const std::wstring& filePath, const std::string& extLower);
    // PrepareGaplessNext()成功後、目標レートを確定してリサンプラーを準備する。
    void ConfirmGaplessTarget(uint32_t targetRate);
    // 事前オープン済みの次曲を破棄する（曲送りをやり直す場合など）。
    // Close()からも自動的に呼ばれる。
    void CancelGapless();

    // デコードスレッドが現在曲のEOFで次曲へシームレスに切り替わった回数。
    // 呼び出し側はこの値の増分をポーリングして検知し、増分を検知したら
    // LastGaplessTransitionInfo()で新トラックの情報を取得してUI状態
    // （再生位置基準・長さ・タグ等）を追いつかせる。
    uint64_t GetGaplessTransitionCount() const { return m_gaplessTransitionCount.load(std::memory_order_acquire); }
    GaplessInfo LastGaplessTransitionInfo() const;

private:
    void DecodeThreadProc();

    std::unique_ptr<IPcmDecoder> m_decoder;
    bool     m_dopMode = false;   // v10: 次のOpen()でDSDをDoPで開くか
    bool     m_isDop   = false;   // v10: 今の曲がDoPか
    uint32_t m_nativeSampleRate = 0;
    uint32_t m_outputSampleRate = 0;
    uint32_t m_channels = 0;
    uint32_t m_bitsPerSample = 0;
    uint64_t m_totalFrames = 0;

    bool m_resamplingActive = false;
    SincResampler m_resampler;

    std::unique_ptr<AudioRingBuffer> m_ring; // interleaved s32

    IAudioOutputBackend* m_outputBackend = nullptr; // 非所有

    std::thread m_decodeThread;
    std::atomic<bool> m_decoding{false};
    std::atomic<bool> m_endOfStream{false};

    // ── ギャップレス：次曲プリロード状態（m_gaplessMutexで保護。UIスレッドが
    //    PrepareGaplessNext/ConfirmGaplessTarget/CancelGaplessで書き込み、
    //    デコードスレッドがEOF到達時にのみ読み取り＆m_decoder等へ移管する）
    mutable std::mutex m_gaplessMutex;
    std::unique_ptr<IPcmDecoder> m_nextDecoder;
    uint32_t m_nextNativeSampleRate = 0;
    uint32_t m_nextBitsPerSample    = 0;
    uint64_t m_nextTotalFrames      = 0;
    bool m_nextResamplingActive     = false;
    SincResampler m_nextResampler;

    // 直近のギャップレス遷移で切り替わった新トラックの情報（m_gaplessMutexで保護）
    uint32_t m_lastTransitionNativeSampleRate = 0;
    uint32_t m_lastTransitionBitsPerSample    = 0;
    uint64_t m_lastTransitionTotalFrames      = 0;
    uint64_t m_lastTransitionBacklogFrames    = 0;

    std::atomic<uint64_t> m_gaplessTransitionCount{0};
};
