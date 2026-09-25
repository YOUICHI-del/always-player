#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

#ifndef ALPHASTYLE_PI
#define ALPHASTYLE_PI 3.14159265358979323846
#endif

// 44.1kHz -> 352.8kHz, fixed 8x upsampling
// taps: filter length (transition band width, recommend 192)
// target_db: target stopband attenuation in dB (recommend 120)
class AlphaStyleInterpolator
{
public:
    explicit AlphaStyleInterpolator(int taps = 192, double target_db = 120.0)
        : m_taps(taps), m_targetDb(target_db)
    {
        buildKernel();
    }

    void process(const float* inL, const float* inR,
                 int samples,
                 std::vector<float>& outL,
                 std::vector<float>& outR)
    {
        const int up = 8;
        outL.resize(samples * up);
        outR.resize(samples * up);
        const int half = m_taps / 2;
        for (int n = 0; n < samples; ++n) {
            for (int k = 0; k < up; ++k) {
                double accL = 0.0;
                double accR = 0.0;
                const auto& phase = m_polyKernel[k];
                for (int i = 0; i < m_taps; ++i) {
                    int idx = n + i - half;
                    if (idx < 0 || idx >= samples) continue;
                    double w = phase[i];
                    accL += (double)inL[idx] * w;
                    accR += (double)inR[idx] * w;
                }
                outL[n * up + k] = (float)accL;
                outR[n * up + k] = (float)accR;
            }
        }
    }

private:
    int m_taps;
    double m_targetDb;
    std::vector<std::vector<double>> m_polyKernel; // [8][taps]

    void buildKernel()
    {
        const int up = 8;
        const double half = m_taps / 2.0;
        const double beta = kaiserBeta(m_targetDb);
        const double N_full = m_taps * (double)up; // fine-grid全体の長さ（窓の基準）

        m_polyKernel.assign(up, std::vector<double>(m_taps));
        for (int phase = 0; phase < up; ++phase) {
            double frac = (double)phase / up;
            double sum = 0.0;
            for (int i = 0; i < m_taps; ++i) {
                double x = frac - ((double)i - half); // 中心からの実オフセット
                double g_index = (x + half) * up;       // fine-grid上の絶対位置
                double w = sinc(x) * kaiserWindowFine(g_index, N_full, beta);
                m_polyKernel[phase][i] = w;
                sum += w;
            }
            for (auto& v : m_polyKernel[phase]) v /= sum;
        }
    }

    static double sinc(double x)
    {
        if (x == 0.0) return 1.0;
        const double pi_x = ALPHASTYLE_PI * x;
        return std::sin(pi_x) / pi_x;
    }

    static double besselI0(double x)
    {
        double sum = 1.0, term = 1.0, xh = x / 2.0;
        for (int k = 1; k < 30; ++k) {
            term *= (xh * xh) / (double)(k * k);
            sum += term;
            if (term < 1e-15 * sum) break;
        }
        return sum;
    }

    static double kaiserBeta(double A)
    {
        if (A > 50.0)
            return 0.1102 * (A - 8.7);
        else if (A >= 21.0)
            return 0.5842 * std::pow(A - 21.0, 0.4) + 0.07886 * (A - 21.0);
        else
            return 0.0;
    }

    // fine-grid(taps*up点)全体を1つの窓とみなして評価
    static double kaiserWindowFine(double g_index, double N_full, double beta)
    {
        double ratio = (2.0 * g_index) / (N_full - 1.0) - 1.0;
        if (std::abs(ratio) > 1.0) return 0.0;
        double arg = beta * std::sqrt((std::max)(0.0, 1.0 - ratio * ratio));
        return besselI0(arg) / besselI0(beta);
    }
};
