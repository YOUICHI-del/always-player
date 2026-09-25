#pragma once
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <vector>

// ─────────────────────────────────────────────────────────
// DspChain
//
// mpv側のafチェーン(Player::applyAudioChain)のうち、サンプルレートを
// 変えない部分だけを新エンジン(FlacDualEngine/WASAPI排他)向けに移植した
// もの。AudioProcessThread::DspCallbackとしてそのまま渡せる形
// （float* interleaved L,R,L,R,... / frameCount、-1.0〜1.0正規化済み）。
//
// ★ dsd8/hires4のアップサンプリングは、DspChainではなくPcmDualEngine側
//   （engine/SincResampler.h、Kaiser窓sinc補間）でデコード直後に行う。
//   DspChain自身は常に「リサンプル後（目標）レート」で動作する同レート
//   処理のみを担当する。
//
// ★ HP補正(bs2b)・音場効果(vibrato/aecho)・ラウドネス正規化は、各アルゴリズム
//   の要点（時定数・フィードバック構造）を踏襲した近似実装であり、
//   ffmpeg/libbs2bとの係数の完全一致は保証しない。
//
// ★ ラウドネス正規化(SetLoudnessOn)について：
//   mpv側のlavfi=loudnorm(I=-14:TP=-1:LRA=11)と同じ目標値（統合ラウドネス
//   -14LUFS、true peak -1dBTP）を狙うが、実装方式は異なる。
//   ・ITU-R BS.1770-4準拠のKウェイティング(shelf+highpass)で測定用信号を作り、
//     時定数付き平均二乗パワーから瞬時LUFSを推定→目標との差分をゲインに変換。
//   ・ゲインはアタック(音量を下げる方向)を速く、リリース(上げる方向)を遅く
//     非対称に平滑化し、ポンピングを抑える（mpvのloudnormのようなルック
//     アヘッド・二passモードではない、リアルタイム単一pass方式）。
//   ・true peak検出はオーバーサンプリングを行わないサンプルピーク基準の
//     簡易リミッタで近似している。LRA(ラウドネスレンジ)制御は行わない。
// ─────────────────────────────────────────────────────────

enum class SoundFieldMode { None, WowFlutter, HallTone };

class DspChain {
public:
    // トラック開始時（tryPlayViaNewEngine成功時）に一度だけ呼ぶ。
    // サンプルレートに応じた各フィルタ係数を計算し、内部状態をリセットする。
    void Prepare(uint32_t sampleRate);

    // 再生中にリアルタイムで切り替え可能（UIスレッドから呼ばれる想定）。
    void SetChainOn(bool on) { m_chainOn.store(on, std::memory_order_relaxed); }
    void SetHp(bool hp1, bool hp2) {
        m_hp1.store(hp1, std::memory_order_relaxed);
        m_hp2.store(hp2, std::memory_order_relaxed);
    }
    void SetSoundField(SoundFieldMode mode) { m_soundField.store(mode, std::memory_order_relaxed); }
    void SetLoudnessOn(bool on) { m_loudnessOn.store(on, std::memory_order_relaxed); }

    // AudioProcessThread::DspCallback互換（DSPスレッドから呼ばれる）
    void Process(float* interleaved, size_t frameCount);

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1[2] = {0, 0};
        double z2[2] = {0, 0};
        float ProcessSample(int ch, float x);
        void Reset() { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
    };
    static void SetPeaking(Biquad &bq, double fs, double f0, double bwOctaves, double dBgain);
    static void SetAllpass(Biquad &bq, double fs, double f0, double q);
    void PrepareLoudnessFilters(double fs);

    uint32_t m_sampleRate = 44100;

    // [1]プレゼンスEQ(f=3200,width=1.5oct,g=0.5dB) [2]高域allpass(f=8000,Q=0.7)
    Biquad m_eq;
    Biquad m_allpass;
    // [3]空芯コイル特性：1極ローパス(f=45000)の近似（指数移動平均）
    double m_lpAlpha = 0.0;
    double m_lpState[2] = {0, 0};
    // [4]偶数次高調波: y = x + 0.03*x^2 （無状態なのでフィールド不要）

    // HP補正(bs2bクロスフィード近似)：cmoy/jmeierそれぞれの係数を事前計算
    double m_bs2bAlphaCmoy = 0.0, m_bs2bGainCmoy = 0.0, m_bs2bFeedCmoy = 0.0;
    double m_bs2bAlphaJmeier = 0.0, m_bs2bGainJmeier = 0.0, m_bs2bFeedJmeier = 0.0;
    double m_bs2bLpState[2] = {0, 0};

    // 音場：ワウフラッター（vibrato近似、f=0.5Hz、d=0.0008）
    std::vector<float> m_vibBuf[2];
    size_t m_vibWritePos = 0;
    size_t m_vibBufLen = 0;
    double m_vibPhase = 0.0;
    double m_vibDepthSamples = 0.0;

    // 音場：ホールトーン（aecho近似、in=0.98,out=0.98,delay=60ms,decay=0.02）
    std::vector<float> m_echoBuf[2];
    size_t m_echoWritePos = 0;
    size_t m_echoBufLen = 0;

    // ラウドネス正規化：BS.1770-4 Kウェイティング(shelf→highpass、測定専用・
    // 出力信号には適用しない)＋平滑化ゲイン制御＋簡易ピークリミッタ
    Biquad m_kShelf;
    Biquad m_kHighpass;
    double m_lufsMsPower          = 1e-10; // 平滑化された平均二乗パワー（下限クランプ済み）
    double m_lufsGainSmoothed     = 1.0;   // 現在適用中のリニアゲイン
    double m_lufsPowerAlpha       = 0.0;   // パワー推定の平滑化係数（時定数3秒相当）
    double m_lufsGainAttackAlpha  = 0.0;   // ゲイン低下方向の平滑化係数（時定数1秒相当）
    double m_lufsGainReleaseAlpha = 0.0;   // ゲイン上昇方向の平滑化係数（時定数5秒相当）

    std::atomic<bool> m_chainOn{true};
    std::atomic<bool> m_hp1{false};
    std::atomic<bool> m_hp2{false};
    std::atomic<SoundFieldMode> m_soundField{SoundFieldMode::None};
    std::atomic<bool> m_loudnessOn{false};
};
