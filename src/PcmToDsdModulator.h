// PcmToDsdModulator.h
//
// PCM(s32) -> DSD(1bit) 変換のベースサンプル。
//
// DSDは非公開のブラックボックス技術ではなく、公知のシグマデルタ変調理論
// （1963年 Inose & Yasuda の論文が起源）をオーディオ向けに応用したもの。
// SACD向けハードウェア認証にはライセンスが必要だが、PCM<->DSD変換の
// アルゴリズム自体は学術的に広く公開されている。
//
// ここでは基本形の「5次シグマデルタ変調器（5th-order noise shaper）」を
// 実装している。DSD64（2.8224MHz）を想定し、44.1kHz系PCMからの
// オーバーサンプリング＋1bit量子化＋ノイズシェーピングを行う。
//
// 設計方針:
//   - あくまでベースサンプル。実運用では以下のチューニングが必要:
//     ノイズシェーピング係数の最適化、過負荷（instability）対策、
//     ディザ付加によるアイドルトーン抑制など
//   - 1chあたり1インスタンス。内部状態（積分器）はプロセス間で
//     リセットしないこと（曲間で状態を引き継がないとノイズが変化する）

#pragma once

#include <cstdint>
#include <vector>
#include <array>
#include <cmath>
#include <algorithm>
#include <random>

class PcmToDsdModulator {
public:
    // oversampleRatio: PCMサンプルレートに対する1bitストリームの倍率
    //                  例) 44100Hz入力でDSD64(2.8224MHz)なら 64
    explicit PcmToDsdModulator(int oversampleRatio = 64)
        : m_ratio(oversampleRatio)
        , m_rng(12345)
        , m_dither(-0.5, 0.5)
    {
        m_integrators.fill(0.0);
    }

    // in  : 1チャンネル分のPCM(s32)。事前に oversampleRatio 倍に
    //       ゼロ次ホールド/線形補間等でアップサンプリング済みの
    //       ものを渡す想定（実際にはこの前段にsinc補間段を挟む）
    // out : 1bit DSDストリーム。1バイト=8サンプル分のビットパック
    void process(const std::vector<double>& oversampledPcm, std::vector<uint8_t>& out) {
        out.assign((oversampledPcm.size() + 7) / 8, 0);

        for (size_t i = 0; i < oversampledPcm.size(); ++i) {
            // 入力レンジを -1.0〜+1.0 に正規化した値を想定
            double x = oversampledPcm[i];

            // フィードバック（前回の1bit出力）を減算しながら
            // 5段のカスケード積分器を通す（5次ノイズシェーピング）
            double v = x - m_lastBit;
            for (int stage = 0; stage < kOrder; ++stage) {
                m_integrators[stage] += v;
                v = m_integrators[stage];
            }

            // 微小ディザでアイドルトーン（無音時の周期的パターンノイズ）を抑制
            double ditheredV = v + m_dither(m_rng) * kDitherAmplitude;

            // 1bit量子化（コンパレータ）
            bool bit = ditheredV >= 0.0;
            m_lastBit = bit ? 1.0 : -1.0;

            if (bit) {
                out[i / 8] |= static_cast<uint8_t>(1 << (7 - (i % 8)));
            }
        }
    }

    void reset() {
        m_integrators.fill(0.0);
        m_lastBit = 0.0;
    }

private:
    static constexpr int kOrder = 5;
    static constexpr double kDitherAmplitude = 1.0 / 32768.0; // 控えめな量

    int m_ratio;
    std::array<double, kOrder> m_integrators{};
    double m_lastBit = 0.0;

    std::mt19937 m_rng;
    std::uniform_real_distribution<double> m_dither;
};

// ---------------------------------------------------------------
// 参考: DSD -> PCM デコード側（デシメーションフィルタ）は
// 1bitストリームにローパスFIRを畳み込んでからダウンサンプリングする、
// という逆方向の処理になります。こちらは既存のFLAC decodeパスとは
// 別のパイプラインとして新設が必要です（別ファイルで用意できます）。
// ---------------------------------------------------------------
