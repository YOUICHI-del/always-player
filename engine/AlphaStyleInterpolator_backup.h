#pragma once
#include <vector>
#include <cmath>
#include <algorithm>

#ifndef ALPHASTYLE_PI
#define ALPHASTYLE_PI 3.14159265358979323846
#endif

// 44.1kHz -> 352.8kHz, fixed 8x upsampling
// taps: filter length (affects transition band width, recommend 192)
// target_db: target stopband attenuation in dB (affects stopband depth, recommend 120)
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
                const double t = (double)n + (double)k / up;
                for (int i = -half; i < half; ++i) {
                    int idx = n + i;
                    if (idx < 0 || idx >= samples) continue;
                    double x = t - (double)idx;
                    double w = kernelAt(x);
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
    std::vector<double> m_kernel;

    void buildKernel()
    {
        m_kernel.resize(m_taps);
        const int half = m_taps / 2;
        const double beta = kaiserBeta(m_targetDb);
        for (int i = 0; i < m_taps; ++i) {
            double x = (double)(i - half);
            m_kernel[i] = sinc(x) * kaiserWindow(i, m_taps, beta);
        }
        double sum = 0.0;
        for (double v : m_kernel) sum += v;
        for (double& v : m_kernel) v /= sum;
    }

    static double sinc(double x)
    {
        if (x == 0.0) return 1.0;
        const double pi_x = ALPHASTYLE_PI * x;
        return std::sin(pi_x) / pi_x;
    }

    static double besselI0(double x)
    {
        double sum = 1.0;
        double term = 1.0;
        double xh = x / 2.0;
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

    static double kaiserWindow(int n, int N, double beta)
    {
        double ratio = (2.0 * n) / (double)(N - 1) - 1.0;
        double arg = beta * std::sqrt((std::max)(0.0, 1.0 - ratio * ratio));
        return besselI0(arg) / besselI0(beta);
    }

    double kernelAt(double x) const
    {
        const int half = m_taps / 2;
        double idx = x + half;
        if (idx < 0.0 || idx > (double)(m_taps - 1)) return 0.0;
        int i0 = (int)idx;
        int i1 = (std::min)(i0 + 1, m_taps - 1);
        double frac = idx - (double)i0;
        return m_kernel[i0] * (1.0 - frac) + m_kernel[i1] * frac;
    }
};
