#include "DspChain.h"
#include <cmath>
#include <algorithm>

namespace {
    constexpr double kPi = 3.14159265358979323846;
}

float DspChain::Biquad::ProcessSample(int ch, float x) {
    // Direct Form II Transposed（a0は正規化済みの前提）
    double in  = static_cast<double>(x);
    double out = b0 * in + z1[ch];
    z1[ch] = b1 * in - a1 * out + z2[ch];
    z2[ch] = b2 * in - a2 * out;
    return static_cast<float>(out);
}

// RBJ Audio EQ Cookbookのピーキングフィルタ（帯域幅はオクターブ指定）
void DspChain::SetPeaking(Biquad &bq, double fs, double f0, double bwOctaves, double dBgain) {
    double w0    = 2.0 * kPi * f0 / fs;
    double A     = std::pow(10.0, dBgain / 40.0);
    double sinw0 = std::sin(w0);
    double cosw0 = std::cos(w0);
    double alpha = sinw0 * std::sinh((std::log(2.0) / 2.0) * bwOctaves * w0 / sinw0);

    double b0 = 1 + alpha * A;
    double b1 = -2 * cosw0;
    double b2 = 1 - alpha * A;
    double a0 = 1 + alpha / A;
    double a1 = -2 * cosw0;
    double a2 = 1 - alpha / A;

    bq.b0 = b0 / a0; bq.b1 = b1 / a0; bq.b2 = b2 / a0;
    bq.a1 = a1 / a0; bq.a2 = a2 / a0;
    bq.Reset();
}

// RBJ Audio EQ Cookbookのallpassフィルタ（Q指定）
void DspChain::SetAllpass(Biquad &bq, double fs, double f0, double q) {
    double w0    = 2.0 * kPi * f0 / fs;
    double alpha = std::sin(w0) / (2.0 * q);
    double cosw0 = std::cos(w0);

    double b0 = 1 - alpha;
    double b1 = -2 * cosw0;
    double b2 = 1 + alpha;
    double a0 = 1 + alpha;
    double a1 = -2 * cosw0;
    double a2 = 1 - alpha;

    bq.b0 = b0 / a0; bq.b1 = b1 / a0; bq.b2 = b2 / a0;
    bq.a1 = a1 / a0; bq.a2 = a2 / a0;
    bq.Reset();
}

// BS.1770-4準拠のKウェイティングフィルタを任意サンプルレート向けに設計する。
// （libebur128等で広く使われる、アナログプロトタイプの双一次変換による
//  一般化式。48kHz固定の標準係数表ではなく、任意fsに対応するためこちらを使う）
void DspChain::PrepareLoudnessFilters(double fs) {
    // Stage 1: プレフィルタ（高域シェルフ）
    {
        const double f0 = 1681.9744509555319;
        const double G  = 3.99984385397;
        const double Q  = 0.7071752369554193;
        const double K  = std::tan(kPi * f0 / fs);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);
        const double a0 = 1.0 + K / Q + K * K;

        m_kShelf.b0 = (Vh + Vb * K / Q + K * K) / a0;
        m_kShelf.b1 = 2.0 * (K * K - Vh) / a0;
        m_kShelf.b2 = (Vh - Vb * K / Q + K * K) / a0;
        m_kShelf.a1 = 2.0 * (K * K - 1.0) / a0;
        m_kShelf.a2 = (1.0 - K / Q + K * K) / a0;
        m_kShelf.Reset();
    }
    // Stage 2: RLB重み付け（高域通過）
    {
        const double f0 = 38.13547087602;
        const double Q  = 0.5003270373238;
        const double K  = std::tan(kPi * f0 / fs);
        const double a0 = 1.0 + K / Q + K * K;

        m_kHighpass.b0 = 1.0;
        m_kHighpass.b1 = -2.0;
        m_kHighpass.b2 = 1.0;
        m_kHighpass.a1 = 2.0 * (K * K - 1.0) / a0;
        m_kHighpass.a2 = (1.0 - K / Q + K * K) / a0;
        m_kHighpass.Reset();
    }
}

void DspChain::Prepare(uint32_t sampleRate) {
    m_sampleRate = (sampleRate > 0) ? sampleRate : 44100;
    double fs = static_cast<double>(m_sampleRate);

    // [1] プレゼンス帯域 EQ
    SetPeaking(m_eq, fs, 3200.0, 1.5, 0.5);
    // [2] 高域allpass
    SetAllpass(m_allpass, fs, 8000.0, 0.7);

    // [3] 空芯コイル特性：1極ローパス(f=45000)の近似
    //     カットオフがナイキスト周波数を超える場合は係数が発散するため、
    //     わずかにナイキスト未満へクランプする。
    double fc = std::min(45000.0, fs / 2.0 * 0.999);
    m_lpAlpha = std::exp(-2.0 * kPi * fc / fs);
    m_lpState[0] = m_lpState[1] = 0.0;

    // v10: 偶数次高調波が生む直流成分を取り除くDCブロッカー(fc=2Hz)
    m_dcR = std::exp(-2.0 * kPi * 2.0 / fs);
    m_dcX1[0] = m_dcX1[1] = 0.0;
    m_dcY1[0] = m_dcY1[1] = 0.0;


    // HP補正(bs2bクロスフィード近似)：cmoy(fcut=700,feed=6.0dB) / jmeier(fcut=650,feed=9.5dB)
    auto setupBs2b = [fs](double fcut, double feedDb,
                           double &alphaOut, double &gainOut, double &feedOut) {
        alphaOut = std::exp(-2.0 * kPi * fcut / fs);
        double level = std::pow(10.0, feedDb / 20.0);
        gainOut = 1.0 / (1.0 + level);
        feedOut = level * gainOut;
    };
    setupBs2b(700.0, 6.0, m_bs2bAlphaCmoy,   m_bs2bGainCmoy,   m_bs2bFeedCmoy);
    setupBs2b(650.0, 9.5, m_bs2bAlphaJmeier, m_bs2bGainJmeier, m_bs2bFeedJmeier);
    m_bs2bLpState[0] = m_bs2bLpState[1] = 0.0;

    // 音場：ワウフラッター（f=0.5Hz、depth=0.0008秒相当）
    m_vibDepthSamples = 0.0008 * fs;
    m_vibBufLen = static_cast<size_t>(m_vibDepthSamples) + 8;
    m_vibBuf[0].assign(m_vibBufLen, 0.0f);
    m_vibBuf[1].assign(m_vibBufLen, 0.0f);
    m_vibWritePos = 0;
    m_vibPhase = 0.0;

    // 音場：ホールトーン（delay=60ms）
    m_echoBufLen = static_cast<size_t>(0.060 * fs) + 1;
    m_echoBuf[0].assign(m_echoBufLen, 0.0f);
    m_echoBuf[1].assign(m_echoBufLen, 0.0f);
    m_echoWritePos = 0;

    // ラウドネス正規化：Kウェイティングフィルタ＋平滑化係数
    PrepareLoudnessFilters(fs);
    constexpr double kPowerTauSec       = 3.0; // パワー推定の時定数
    constexpr double kGainAttackTauSec  = 1.0; // ゲイン低下（音量を下げる）：速め
    constexpr double kGainReleaseTauSec = 5.0; // ゲイン上昇（音量を上げる）：遅め、ポンピング防止
    m_lufsPowerAlpha       = std::exp(-1.0 / (kPowerTauSec * fs));
    m_lufsGainAttackAlpha  = std::exp(-1.0 / (kGainAttackTauSec * fs));
    m_lufsGainReleaseAlpha = std::exp(-1.0 / (kGainReleaseTauSec * fs));
    m_lufsMsPower      = 1e-10;
    m_lufsGainSmoothed = 1.0;
}

void DspChain::Process(float* interleaved, size_t frameCount) {
    const bool chainOn = m_chainOn.load(std::memory_order_relaxed);
    const bool hp1 = m_hp1.load(std::memory_order_relaxed);
    const bool hp2 = m_hp2.load(std::memory_order_relaxed);
    const SoundFieldMode sf = m_soundField.load(std::memory_order_relaxed);
    const bool loudnessOn = m_loudnessOn.load(std::memory_order_relaxed);

    constexpr double kTargetLufs      = -14.0;      // mpv側 lavfi=loudnorm=I=-14 と同一目標
    constexpr float  kTargetPeakLinear = 0.89125094f; // -1dBTP相当（mpv側 TP=-1 と同一目標）

    for (size_t i = 0; i < frameCount; ++i) {
        float l = interleaved[i * 2 + 0];
        float r = interleaved[i * 2 + 1];

        // ── ラウドネス正規化（BS.1770-4 Kウェイティング測定＋平滑化ゲイン）
        if (loudnessOn) {
            // 測定専用のKウェイティング信号（出力信号そのものは変更しない）
            float kl = m_kShelf.ProcessSample(0, l);
            kl = m_kHighpass.ProcessSample(0, kl);
            float kr = m_kShelf.ProcessSample(1, r);
            kr = m_kHighpass.ProcessSample(1, kr);

            const double instPower = (static_cast<double>(kl) * kl + static_cast<double>(kr) * kr) * 0.5;
            m_lufsMsPower = m_lufsPowerAlpha * m_lufsMsPower + (1.0 - m_lufsPowerAlpha) * instPower;
            if (m_lufsMsPower < 1e-10) m_lufsMsPower = 1e-10;

            const double lufs = -0.691 + 10.0 * std::log10(m_lufsMsPower);
            double gainDb = kTargetLufs - lufs;
            gainDb = std::clamp(gainDb, -24.0, 24.0);
            const double targetGain = std::pow(10.0, gainDb / 20.0);

            const double alpha = (targetGain < m_lufsGainSmoothed) ? m_lufsGainAttackAlpha : m_lufsGainReleaseAlpha;
            m_lufsGainSmoothed = alpha * m_lufsGainSmoothed + (1.0 - alpha) * targetGain;

            l = static_cast<float>(l * m_lufsGainSmoothed);
            r = static_cast<float>(r * m_lufsGainSmoothed);

            // 簡易true peak安全リミッタ（オーバーサンプリングなし、サンプルピーク基準の近似）
            const float peak = std::max(std::fabs(l), std::fabs(r));
            if (peak > kTargetPeakLinear) {
                const float reduce = kTargetPeakLinear / peak;
                l *= reduce;
                r *= reduce;
            }
        }

        // ── 中密度チェーン：EQ→allpass→lowpass→偶数次高調波
        if (chainOn) {
            l = m_eq.ProcessSample(0, l);
            r = m_eq.ProcessSample(1, r);
            l = m_allpass.ProcessSample(0, l);
            r = m_allpass.ProcessSample(1, r);

            m_lpState[0] = (1.0 - m_lpAlpha) * l + m_lpAlpha * m_lpState[0];
            m_lpState[1] = (1.0 - m_lpAlpha) * r + m_lpAlpha * m_lpState[1];
            l = static_cast<float>(m_lpState[0]);
            r = static_cast<float>(m_lpState[1]);

            l = l + 0.03f * l * l;
            r = r + 0.03f * r * r;

            // ★ v10: DCブロッカー（1次ハイパス、fc=2Hz）。
            //   x²の項は偶数次高調波と同時に「音量に比例した直流成分」も生む
            //   （実測：約-42dB＝フルスケールの約0.75%）。直流はスピーカーや
            //   ヘッドホンの振動板を片側へ押し続けるだけの無駄な成分なので、
            //   ここで取り除く。2Hzなら可聴帯域への影響は20Hzで-0.04dBのみで、
            //   付加した2次高調波そのものは変わらない（シミュレーションで確認）。
            {
                const double yl = static_cast<double>(l) - m_dcX1[0] + m_dcR * m_dcY1[0];
                const double yr = static_cast<double>(r) - m_dcX1[1] + m_dcR * m_dcY1[1];
                m_dcX1[0] = l;  m_dcY1[0] = yl;
                m_dcX1[1] = r;  m_dcY1[1] = yr;
                l = static_cast<float>(yl);
                r = static_cast<float>(yr);
            }
        }

        // ── HP補正（cmoy / jmeier、どちらか一方のみ有効）
        if (hp1 || hp2) {
            const double alpha = hp1 ? m_bs2bAlphaCmoy   : m_bs2bAlphaJmeier;
            const double gain  = hp1 ? m_bs2bGainCmoy    : m_bs2bGainJmeier;
            const double feed  = hp1 ? m_bs2bFeedCmoy    : m_bs2bFeedJmeier;

            m_bs2bLpState[0] = alpha * m_bs2bLpState[0] + (1.0 - alpha) * l;
            m_bs2bLpState[1] = alpha * m_bs2bLpState[1] + (1.0 - alpha) * r;

            const double outL = gain * l + feed * m_bs2bLpState[1];
            const double outR = gain * r + feed * m_bs2bLpState[0];
            l = static_cast<float>(outL);
            r = static_cast<float>(outR);
        }

        // ── 音場効果（ワウフラッター / ホールトーン、どちらか一方のみ）
        if (sf == SoundFieldMode::WowFlutter) {
            m_vibPhase += 2.0 * kPi * 0.5 / m_sampleRate;
            if (m_vibPhase > 2.0 * kPi) m_vibPhase -= 2.0 * kPi;
            const double mod = (std::sin(m_vibPhase) + 1.0) * 0.5 * m_vibDepthSamples;

            m_vibBuf[0][m_vibWritePos] = l;
            m_vibBuf[1][m_vibWritePos] = r;

            double readPosF = static_cast<double>(m_vibWritePos) - mod;
            while (readPosF < 0.0) readPosF += static_cast<double>(m_vibBufLen);
            const size_t readPos0 = static_cast<size_t>(readPosF) % m_vibBufLen;
            const size_t readPos1 = (readPos0 + 1) % m_vibBufLen;
            const double frac = readPosF - std::floor(readPosF);

            l = static_cast<float>(m_vibBuf[0][readPos0] * (1.0 - frac) + m_vibBuf[0][readPos1] * frac);
            r = static_cast<float>(m_vibBuf[1][readPos0] * (1.0 - frac) + m_vibBuf[1][readPos1] * frac);

            m_vibWritePos = (m_vibWritePos + 1) % m_vibBufLen;
        } else if (sf == SoundFieldMode::HallTone) {
            constexpr double kInGain = 0.98, kOutGain = 0.98, kDecay = 0.02;
            const float delayedL = m_echoBuf[0][m_echoWritePos];
            const float delayedR = m_echoBuf[1][m_echoWritePos];

            const double mixedL = kInGain * l + kDecay * delayedL;
            const double mixedR = kInGain * r + kDecay * delayedR;

            m_echoBuf[0][m_echoWritePos] = static_cast<float>(mixedL);
            m_echoBuf[1][m_echoWritePos] = static_cast<float>(mixedR);

            l = static_cast<float>(kOutGain * mixedL);
            r = static_cast<float>(kOutGain * mixedR);

            m_echoWritePos = (m_echoWritePos + 1) % m_echoBufLen;
        }

        interleaved[i * 2 + 0] = l;
        interleaved[i * 2 + 1] = r;
    }
}
