// SplineInterpolator.h
//
// 3次スプライン（Catmull-Romスプライン）によるアップサンプラー。
// AlphaStyleInterpolator.h（Kaiser窓sinc補間）と同じインターフェースにしてあるので、
// DSPチェーン内で差し替えて聴き比べができます。
//
// sinc補間との違い:
//   - 周波数領域の「帯域制限理論」ではなく、区間ごとの3次多項式で
//     滑らかに波形を繋ぐ幾何学的アプローチ。
//   - タップ数が少なくて済む（常に前後2点ずつ、計4点）ため計算が軽い。
//   - sinc系で出やすいリンギングが原理的に出にくい。
//   - 一方で高域の理論的な再現精度はsinc系に劣る可能性がある。
//     （このへんは実際に試聴して判断するのが一番速いです）
//
// 設計方針はAlphaStyleInterpolatorと同じ:
//   - コア経路はs32のまま。float変換はこのステージ内だけ。
//   - BitPerfect ON時は丸ごとバイパス可能な構造。
//   - チャンネルごとに1インスタンス、履歴サンプルのみ保持。

#pragma once

#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>

class SplineInterpolator {
public:
    // oversample : オーバーサンプル比（2, 4, 8 など）
    // tension    : Catmull-Romのテンション係数。0.0が標準（滑らかさ重視）、
    //              1.0に近づけるほど鋭さが増す（波形の角が立ちやすくなる）
    explicit SplineInterpolator(int oversample = 4, double tension = 0.0)
        : m_oversample(oversample)
        , m_tension(tension)
        , m_history(4, 0.0) // p(-1), p0, p1, p2 の4点だけ保持
    {}

    // BitPerfect ON時はこの関数を呼ばない設計にしてください。
    //
    // in  : 1チャンネル分の s32 PCM
    // out : オーバーサンプルされた s32 PCM（サイズは in.size() * oversample）
    void process(const std::vector<int32_t>& in, std::vector<int32_t>& out) {
        out.resize(in.size() * static_cast<size_t>(m_oversample));

        for (size_t i = 0; i < in.size(); ++i) {
            // 履歴を1つ進める: [p(-1), p0, p1, p2] を1サンプル分シフト
            m_history[0] = m_history[1];
            m_history[1] = m_history[2];
            m_history[2] = m_history[3];
            m_history[3] = static_cast<double>(in[i]);

            // p0-p1区間を補間する（p(-1)とp2は接線推定に使う周辺データ）
            double p_1 = m_history[0];
            double p0  = m_history[1];
            double p1  = m_history[2];
            double p2  = m_history[3];

            for (int phase = 0; phase < m_oversample; ++phase) {
                double t = static_cast<double>(phase) / m_oversample;
                double v = catmullRom(p_1, p0, p1, p2, t);
                out[i * m_oversample + phase] = clampToS32(v);
            }
        }
    }

    void reset() {
        std::fill(m_history.begin(), m_history.end(), 0.0);
    }

private:
    int m_oversample;
    double m_tension;
    std::vector<double> m_history;

    // Catmull-Romスプライン: p0とp1の間をtで補間。
    // 前後の点(p_1, p2)から接線（傾き）を推定することで、
    // 区間の継ぎ目で波形が滑らかに繋がるようにする。
    double catmullRom(double p_1, double p0, double p1, double p2, double t) const {
        double s = 1.0 - m_tension;
        double m0 = s * (p1 - p_1) * 0.5; // p0点での接線
        double m1 = s * (p2 - p0) * 0.5;  // p1点での接線

        double t2 = t * t;
        double t3 = t2 * t;

        double h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
        double h10 = t3 - 2.0 * t2 + t;
        double h01 = -2.0 * t3 + 3.0 * t2;
        double h11 = t3 - t2;

        return h00 * p0 + h10 * m0 + h01 * p1 + h11 * m1;
    }

    static int32_t clampToS32(double v) {
        double r = std::round(v);
        if (r > 2147483647.0) return 2147483647;
        if (r < -2147483648.0) return -2147483648;
        return static_cast<int32_t>(r);
    }
};
