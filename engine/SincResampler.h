#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>

// ─────────────────────────────────────────────────────────
// SincResampler
//
// 新エンジン(PcmDualEngine)向けの高品質リアルタイムリサンプラー。
// Kaiser窓を用いたwindowed-sincポリフェーズ補間により、任意の
// レート比（整数比に限らない。例：48000→352800=7.35倍）に対応する。
//
// ・カーネル：半長 kHalfTaps=32（全長65タップ）、1024フェーズに量子化
//   （フェーズ間の線形補間は行わない。1024分割の量子化誤差は
//    十分低く、実用上聴感に影響しない前提の割り切った設計）。
// ・カットオフ：source/targetのナイキストの低い方の95%
//   （アップサンプリング時は原音ナイキストのみ、ダウンサンプリング時は
//    エイリアシング防止のため出力側ナイキストも考慮）。
// ・各フェーズのタップ係数はDCゲインが1になるよう正規化済み
//   （窓関数による打ち切り誤差を吸収し、音量ズレを防ぐ）。
//
// 入出力はinterleaved int32（左詰め正規化済み、dr_flac等と同じ規約）。
// チャンネル数は2（ステレオ）固定。
// ─────────────────────────────────────────────────────────
class SincResampler {
public:
    // sourceRate→targetRateへのリサンプルを準備する。
    // targetRate==sourceRateの場合でも呼び出しは可能（実質恒等変換になる）。
    void Prepare(double sourceRate, double targetRate);

    // Prepare()と同じフィルタ係数のまま、内部の履歴バッファ・読み出し位置
    // だけをリセットする。シーク直後など、連続性が失われた場合に呼ぶ。
    void Reset();

    // in: interleaved int32, inFrames個を入力として取り込み、生成できた分
    // だけoutへ書き出す。out側の容量はoutCapacityFramesフレーム分。
    // 戻り値：実際に書き出したフレーム数。
    // ★ 入力に対して出力が0フレームになることもある（内部バッファに
    //   まだ十分な先読みが溜まっていない場合）。これは正常動作。
    size_t Process(const int32_t* in, size_t inFrames, int32_t* out, size_t outCapacityFrames);

    // ストリーム終端で、内部に取り込み済みだが未出力の末尾分を
    // ゼロパディングして書き出す。トラック終端の欠落を防ぐ。
    size_t Flush(int32_t* out, size_t outCapacityFrames);

    // 現在のレート比（source/target）。DecodeThreadProc側で出力バッファの
    // 必要サイズを見積もるのに使う。
    double GetStep() const { return m_step; }

private:
    static constexpr int kNumPhases = 1024;
    static constexpr int kHalfTaps  = 32;
    static constexpr int kFullTaps  = 2 * kHalfTaps + 1;

    void BuildPhaseTable(double sourceRate, double targetRate);
    size_t ProduceFrom(int32_t* out, size_t outCapacityFrames, size_t producibleLimit);

    std::vector<float> m_phaseTable; // [phase * kFullTaps + tap]

    double m_step = 1.0;        // 1出力サンプルあたりの入力側の進み幅（source-sample単位）
    double m_posInBuffer = 0.0; // m_bufferL/R内での現在の読み出し位置（浮動小数点）

    std::vector<float> m_bufferL;
    std::vector<float> m_bufferR;

    bool m_flushed = false; // Flush()呼び出し済みフラグ（多重Flush防止）
};
