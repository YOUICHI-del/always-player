// AlphaStyleInterpolator.h
//
// Denon Alpha Processor系（ALPHA -> AL24 -> Advanced AL32 -> Ultra AL32）の
// 「量子化で失われた点を、前後の多数のサンプルから高次推定で導き出す」という
// 発想を、一般的なDSP理論（窓関数付きsinc補間 / ポリフェーズフィルタ）で
// 近似したベースサンプル。
//
// Denon独自の実アルゴリズム・実装ではなく、公知のDSP技法による再構成です。
//
// 設計方針（Always Player v8のアーキテクチャに合わせています）:
//   - コア経路は s32 のまま。float変換はこのステージに入る直前/出る直後のみ。
//   - BitPerfect ON時はこのステージ自体を丸ごとバイパスできる構造。
//   - 単一スレッド（AudioProcessThread）内で1チャンネルずつ、またはL/R交互に
//     呼び出す想定。内部状態は履歴サンプルのみなので、チャンネルごとに
//     インスタンスを分ければそのまま両チャンネル使い回せます。
//
// 次にやること（v9で詰める部分）:
//   - タップ数・カットオフ・Kaiserベータ値のチューニング（試聴で追い込む）
//   - オーバーサンプル比（2x/4x/8x）を再生モードごとに切替えるロジック
//   - 出力ビット深度を下げる場合のノイズシェーピング付きディザ（今は未実装）
//   - 「周辺データから導く」度合いを強める＝タップ数を増やす、程度は
//     レイテンシとのトレードオフなので要検証

#pragma once

#include <cstdint>
#include <vector>
#include <cmath>
#include <algorithm>

class AlphaStyleInterpolator {
public:
    // taps         : フィルタのタップ数（多いほど「周辺多数のデータから
    //                導き出す」度合いが強くなるが、レイテンシとCPU負荷も増える）
    //                まずは 32 か 64 あたりから試聴推奨
    // oversample   : オーバーサンプル比（2, 4, 8 など）
    // cutoffRatio  : ナイキスト周波数に対するカットオフ比（0.90〜0.98あたり）
    // kaiserBeta   : Kaiser窓のベータ値（大きいほどストップバンド減衰が強く
    //                なるが、遷移帯域が緩やかになる。8.0〜10.0が目安）
    AlphaStyleInterpolator(int taps = 32,
                            int oversample = 4,
                            double cutoffRatio = 0.94,
                            double kaiserBeta = 9.0)
        : m_taps(taps)
        , m_oversample(oversample)
        , m_history(taps, 0.0)
    {
        buildPolyphaseTable(cutoffRatio, kaiserBeta);
    }

    // BitPerfect ON時はこの関数自体を呼ばない設計にしてください（完全バイパス）。
    //
    // in  : 1チャンネル分の s32 PCM（正規化前の生の整数値のまま渡してOK）
    // out : オーバーサンプルされた s32 PCM（サイズは in.size() * oversample）
    void process(const std::vector<int32_t>& in, std::vector<int32_t>& out) {
        out.resize(in.size() * static_cast<size_t>(m_oversample));

        // s32 -> double はこのステージ内だけの一時変換。
        // コア経路のs32運用ポリシーは崩さない。
        for (size_t i = 0; i < in.size(); ++i) {
            // 履歴バッファをシフトして最新サンプルを追加
            m_history.erase(m_history.begin());
            m_history.push_back(static_cast<double>(in[i]));

            for (int phase = 0; phase < m_oversample; ++phase) {
                double acc = 0.0;
                const auto& coeffs = m_polyphaseTable[phase];
                // 「前後多数のデータからあるべき点を導く」部分：
                // 履歴バッファ全体（=周辺サンプル群）に対して
                // 位相ごとの重み付き畳み込みを行う
                for (int t = 0; t < m_taps; ++t) {
                    acc += m_history[t] * coeffs[t];
                }
                out[i * m_oversample + phase] = clampToS32(acc);
            }
        }
    }

    void reset() {
        std::fill(m_history.begin(), m_history.end(), 0.0);
    }

private:
    int m_taps;
    int m_oversample;
    std::vector<double> m_history;
    std::vector<std::vector<double>> m_polyphaseTable; // [phase][tap]

    static double sinc(double x) {
        if (std::fabs(x) < 1e-9) return 1.0;
        double px = M_PI * x;
        return std::sin(px) / px;
    }

    // Kaiser窓（I0ベッセル関数の級数近似）
    static double besselI0(double x) {
        double sum = 1.0, term = 1.0;
        double xh = x / 2.0;
        for (int k = 1; k < 25; ++k) {
            term *= (xh / k) * (xh / k);
            sum += term;
        }
        return sum;
    }

    double kaiserWindow(double n, double N, double beta) const {
        double r = (2.0 * n / (N - 1)) - 1.0;
        double arg = beta * std::sqrt(std::max(0.0, 1.0 - r * r));
        return besselI0(arg) / besselI0(beta);
    }

    // 位相ごとにフィルタ係数を事前計算（ポリフェーズ分解）
    // これにより実行時は「テーブル参照＋畳み込み」だけで済み、
    // アップサンプル比を上げてもリアルタイム性を保ちやすい。
    void buildPolyphaseTable(double cutoffRatio, double kaiserBeta) {
        m_polyphaseTable.assign(m_oversample, std::vector<double>(m_taps, 0.0));
        int center = m_taps / 2;

        for (int phase = 0; phase < m_oversample; ++phase) {
            double sum = 0.0;
            for (int t = 0; t < m_taps; ++t) {
                double x = (t - center) - static_cast<double>(phase) / m_oversample;
                double h = sinc(x * cutoffRatio) * cutoffRatio;
                double w = kaiserWindow(t, m_taps, kaiserBeta);
                double coeff = h * w;
                m_polyphaseTable[phase][t] = coeff;
                sum += coeff;
            }
            // ゲイン正規化（無音時のDC誤差防止）
            if (std::fabs(sum) > 1e-9) {
                for (auto& c : m_polyphaseTable[phase]) c /= sum;
            }
        }
    }

    static int32_t clampToS32(double v) {
        double r = std::round(v);
        if (r > 2147483647.0) return 2147483647;
        if (r < -2147483648.0) return -2147483648;
        return static_cast<int32_t>(r);
    }
};
