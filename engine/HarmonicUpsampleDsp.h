#pragma once
#include <cstddef>

// 倍音付加DSP（tanhソフトサチュレーションのdry/wetミックス）。
// 試聴優先のためまずシンプルな実装。位相・タイミングは変更しない
// （サンプルごとの独立処理のため）。
class HarmonicUpsampleDsp {
public:
    explicit HarmonicUpsampleDsp(float harmonicAmount = 0.15f);

    // AudioProcessThread::DspCallbackとして渡す。
    // interleavedSamples: L,R,L,R,... のfloat配列（-1.0〜1.0正規化済み）
    void Process(float* interleavedSamples, size_t frameCount);

    void SetHarmonicAmount(float amount) { m_harmonicAmount = amount; }
    float GetHarmonicAmount() const { return m_harmonicAmount; }

private:
    float m_harmonicAmount;
};
