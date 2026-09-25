#include "SincResampler.h"
#include <cmath>
#include <algorithm>

namespace {
    constexpr double kPi = 3.14159265358979323846;

    // 0次変形ベッセル関数 I0(x) の近似（Abramowitz & Stegun 9.8.1/9.8.2）。
    // Kaiser窓の計算に使用。相対誤差 1.6e-7 程度で実用上十分な精度。
    double BesselI0(double x) {
        const double ax = std::fabs(x);
        if (ax < 3.75) {
            const double t = x / 3.75;
            const double t2 = t * t;
            return 1.0 + t2 * (3.5156229 + t2 * (3.0899424 + t2 * (1.2067492 +
                   t2 * (0.2659732 + t2 * (0.0360768 + t2 * 0.0045813)))));
        } else {
            const double t = 3.75 / ax;
            return (std::exp(ax) / std::sqrt(ax)) * (0.39894228 + t * (0.01328592 +
                   t * (0.00225319 + t * (-0.00157565 + t * (0.00916281 + t * (-0.02057706 +
                   t * (0.02635537 + t * (-0.01647633 + t * 0.00392377))))))));
        }
    }
}

void SincResampler::Prepare(double sourceRate, double targetRate) {
    if (sourceRate <= 0.0) sourceRate = 44100.0;
    if (targetRate <= 0.0) targetRate = sourceRate;
    m_step = sourceRate / targetRate;
    BuildPhaseTable(sourceRate, targetRate);
    Reset();
}

void SincResampler::BuildPhaseTable(double sourceRate, double targetRate) {
    // カットオフ：source/targetのナイキストの低い方の95%（source-sample単位、cycles/sample）。
    // アップサンプリング時はtargetRate>sourceRateなのでmin(...)=1.0となり、
    // 実質「原音のナイキストのみ」で制限される（帯域を削らない）。
    // ダウンサンプリング時（targetRate<sourceRate）はエイリアシング防止のため
    // 出力側ナイキストで絞る。
    const double nyquistRatio = std::min(1.0, targetRate / sourceRate);
    const double cutoff = 0.5 * nyquistRatio * 0.95;
    constexpr double kBeta = 8.6; // Kaiser窓β。ストップバンド抑圧 約90dB相当

    m_phaseTable.assign(static_cast<size_t>(kNumPhases) * kFullTaps, 0.0f);
    const double i0Beta = BesselI0(kBeta);

    for (int p = 0; p < kNumPhases; ++p) {
        const double frac = static_cast<double>(p) / kNumPhases; // [0, 1)
        std::vector<double> w(kFullTaps);
        double sum = 0.0;

        for (int k = 0; k < kFullTaps; ++k) {
            const int j = k - kHalfTaps;      // [-kHalfTaps, +kHalfTaps]
            const double t = j - frac;         // 連続時間オフセット（source-sample単位）

            // 理想ローパスの連続インパルス応答：h(t) = 2*Fc*sinc(2*Fc*t)
            const double x = 2.0 * cutoff * t;
            const double sincVal = (std::fabs(x) < 1e-9) ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double h = 2.0 * cutoff * sincVal;

            // Kaiser窓（有限区間[-kHalfTaps,+kHalfTaps]でなだらかに0へ収束させる）
            const double windowArg = t / static_cast<double>(kHalfTaps);
            double win = 0.0;
            if (std::fabs(windowArg) <= 1.0) {
                win = BesselI0(kBeta * std::sqrt(std::max(0.0, 1.0 - windowArg * windowArg))) / i0Beta;
            }

            w[k] = h * win;
            sum += w[k];
        }

        // DCゲインを1に正規化（窓による打ち切り誤差を吸収し、音量ズレを防ぐ）
        if (std::fabs(sum) > 1e-12) {
            for (int k = 0; k < kFullTaps; ++k) w[k] /= sum;
        }
        for (int k = 0; k < kFullTaps; ++k) {
            m_phaseTable[static_cast<size_t>(p) * kFullTaps + k] = static_cast<float>(w[k]);
        }
    }
}

void SincResampler::Reset() {
    // 先頭にkHalfTaps分のゼロパディングを置くことで、トラック先頭・シーク直後
    // でも「左側の文脈が足りない」特別扱いをせずに済むようにする。
    m_bufferL.assign(kHalfTaps, 0.0f);
    m_bufferR.assign(kHalfTaps, 0.0f);
    m_posInBuffer = static_cast<double>(kHalfTaps);
    m_flushed = false;
}

size_t SincResampler::ProduceFrom(int32_t* out, size_t outCapacityFrames, size_t /*unused*/) {
    const size_t bufSize = m_bufferL.size();
    size_t produced = 0;

    while (produced < outCapacityFrames) {
        const int64_t i0 = static_cast<int64_t>(std::floor(m_posInBuffer));
        if (i0 < static_cast<int64_t>(kHalfTaps)) break; // 万一の下限割れガード
        if (i0 + kHalfTaps > static_cast<int64_t>(bufSize) - 1) break; // 右側の文脈が不足

        const double frac = m_posInBuffer - static_cast<double>(i0);
        int phase = static_cast<int>(frac * kNumPhases + 0.5);
        if (phase >= kNumPhases) phase = kNumPhases - 1;
        const float* tap = &m_phaseTable[static_cast<size_t>(phase) * kFullTaps];

        double accL = 0.0, accR = 0.0;
        const int64_t base = i0 - kHalfTaps;
        for (int k = 0; k < kFullTaps; ++k) {
            const float w = tap[k];
            accL += static_cast<double>(m_bufferL[static_cast<size_t>(base + k)]) * w;
            accR += static_cast<double>(m_bufferR[static_cast<size_t>(base + k)]) * w;
        }

        auto clampToS32 = [](double v) -> int32_t {
            if (v > 2147483647.0)  v = 2147483647.0;
            if (v < -2147483648.0) v = -2147483648.0;
            return static_cast<int32_t>(v);
        };
        out[produced * 2 + 0] = clampToS32(accL);
        out[produced * 2 + 1] = clampToS32(accR);
        ++produced;

        m_posInBuffer += m_step;
    }

    // 使い終えた先頭部分を捨てて履歴バッファを圧縮する
    // （kHalfTaps分は次回の左側文脈として残す）
    const int64_t curI0 = static_cast<int64_t>(std::floor(m_posInBuffer));
    const int64_t dropCount = curI0 - static_cast<int64_t>(kHalfTaps);
    if (dropCount > 0 && static_cast<size_t>(dropCount) <= m_bufferL.size()) {
        m_bufferL.erase(m_bufferL.begin(), m_bufferL.begin() + dropCount);
        m_bufferR.erase(m_bufferR.begin(), m_bufferR.begin() + dropCount);
        m_posInBuffer -= static_cast<double>(dropCount);
    }

    return produced;
}

size_t SincResampler::Process(const int32_t* in, size_t inFrames, int32_t* out, size_t outCapacityFrames) {
    if (inFrames > 0) {
        const size_t oldSize = m_bufferL.size();
        m_bufferL.resize(oldSize + inFrames);
        m_bufferR.resize(oldSize + inFrames);
        for (size_t f = 0; f < inFrames; ++f) {
            m_bufferL[oldSize + f] = static_cast<float>(in[f * 2 + 0]);
            m_bufferR[oldSize + f] = static_cast<float>(in[f * 2 + 1]);
        }
    }
    return ProduceFrom(out, outCapacityFrames, 0);
}

size_t SincResampler::Flush(int32_t* out, size_t outCapacityFrames) {
    if (m_flushed) return 0;
    m_flushed = true;

    // 末尾にkHalfTaps分のゼロパディングを追加し、既に取り込み済みの
    // 最後のサンプルまで出力しきる。
    const size_t oldSize = m_bufferL.size();
    m_bufferL.resize(oldSize + kHalfTaps, 0.0f);
    m_bufferR.resize(oldSize + kHalfTaps, 0.0f);
    return ProduceFrom(out, outCapacityFrames, 0);
}
