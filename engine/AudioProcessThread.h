#pragma once
#include "RingBuffer.h"
#include "IAudioOutputBackend.h"
#include <thread>
#include <atomic>
#include <vector>
#include <functional>

class AudioProcessThread {
public:
    using DspCallback = std::function<void(float* interleavedSamples, size_t frameCount)>;
    // ★ 「もうこれ以上リングバッファへの書き込みは来ない」かどうかを問い合わせる
    //   コールバック（PcmDualEngine::IsEndOfStream()相当）。ギャップレス遷移中は
    //   falseのままなので、1チャンク未満の状態でも次曲のデータを待って正しく
    //   ブロックし続ける。真の終端(このコールバックがtrueを返す)でのみ、
    //   残っている端数フレームをゼロ埋めして最後の1回だけ出力する
    //   （でないと端数がリングバッファに残り続け、AvailableToRead()が
    //    永久に0にならず、呼び出し側のEOF検知が止まってしまう）。
    using EofQuery = std::function<bool()>;

    AudioProcessThread(AudioRingBuffer* sourceRing, IAudioOutputBackend* backend);
    ~AudioProcessThread();

    void SetDspCallback(DspCallback callback);
    void SetBitPerfect(bool enabled);
    void SetEofQuery(EofQuery query) { m_eofQuery = std::move(query); }
    // ★ v10: 音量（リニア倍率 0.0〜1.0）。1.0のときは一切計算しない
    //   （ビットパーフェクトのまま素通し）。1.0未満のときだけ倍精度で掛ける。
    void SetGain(double gain) { m_gain.store(gain, std::memory_order_relaxed); }
    // ★ v10.0.x: ノイズシェーピング（疑似DSD×8で有効）。DSP経路の最終段で、
    //   出力機器の有効ビット数へTPDFディザ＋2次ノイズシェーピングで量子化する。
    //   ビットパーフェクト経路では一切使わない（値を変えないため）。
    void SetNoiseShaping(bool on) { m_noiseShaping.store(on, std::memory_order_relaxed); }

    void Start();
    void Stop();
    bool IsRunning() const { return m_running.load(); }

private:
    void ThreadProc();

    AudioRingBuffer* m_sourceRing;
    IAudioOutputBackend* m_backend;

    DspCallback m_dspCallback;
    EofQuery m_eofQuery;
    std::atomic<bool> m_bitPerfect{true};
    std::atomic<double> m_gain{1.0};
    std::atomic<bool> m_noiseShaping{false};
    std::atomic<bool> m_running{false};
    std::thread m_thread;

    size_t m_chunkFrames = 0;
    bool m_finalPartialFlushed = false; // 真の終端の端数フレームを出力済みか（多重出力防止）
};
