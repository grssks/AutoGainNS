#pragma once

#include <cmath>
#include <algorithm>

namespace autogain {

/**
 * 64-bit Direct Form II Transposed Biquad filter (zero dynamic allocation).
 */
class Biquad {
public:
    Biquad() { reset(); }

    void reset() noexcept {
        z1_ = 0.0;
        z2_ = 0.0;
    }

    void setBypass() noexcept {
        b0_ = 1.0; b1_ = 0.0; b2_ = 0.0;
        a1_ = 0.0; a2_ = 0.0;
    }

    /**
     * 2nd-order Butterworth / Q-configurable High-Pass Filter (RBJ Audio EQ Cookbook).
     */
    void setHighPass(double sampleRate, double freqHz, double q = 0.7071067811865475) noexcept {
        if (sampleRate <= 0.0 || freqHz <= 0.0) {
            setBypass();
            return;
        }
        const double fc = std::clamp(freqHz, 5.0, sampleRate * 0.45);
        const double w0 = 2.0 * kPi * fc / sampleRate;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double alpha = sw / (2.0 * std::max(q, 0.1));

        const double a0 = 1.0 + alpha;
        const double invA0 = 1.0 / a0;
        b0_ = ((1.0 + cw) * 0.5) * invA0;
        b1_ = (-(1.0 + cw)) * invA0;
        b2_ = ((1.0 + cw) * 0.5) * invA0;
        a1_ = (-2.0 * cw) * invA0;
        a2_ = (1.0 - alpha) * invA0;
    }

    /**
     * 2nd-order Butterworth / Q-configurable Low-Pass Filter (RBJ Audio EQ Cookbook).
     */
    void setLowPass(double sampleRate, double freqHz, double q = 0.7071067811865475) noexcept {
        if (sampleRate <= 0.0 || freqHz <= 0.0) {
            setBypass();
            return;
        }
        const double fc = std::clamp(freqHz, 5.0, sampleRate * 0.45);
        const double w0 = 2.0 * kPi * fc / sampleRate;
        const double cw = std::cos(w0);
        const double sw = std::sin(w0);
        const double alpha = sw / (2.0 * std::max(q, 0.1));

        const double a0 = 1.0 + alpha;
        const double invA0 = 1.0 / a0;
        b0_ = ((1.0 - cw) * 0.5) * invA0;
        b1_ = (1.0 - cw) * invA0;
        b2_ = ((1.0 - cw) * 0.5) * invA0;
        a1_ = (-2.0 * cw) * invA0;
        a2_ = (1.0 - alpha) * invA0;
    }

    /**
     * ITU-R BS.1770-4 K-weighting Stage 1: High-shelf pre-filter (+4 dB @ ~1681 Hz).
     * Uses exact analog prototype + bilinear transform for arbitrary sample rates.
     */
    void setKWeightingStage1(double sampleRate) noexcept {
        if (sampleRate <= 0.0) {
            setBypass();
            return;
        }
        constexpr double f0 = 1681.974450955533;
        constexpr double G = 3.999843853973347;
        constexpr double Q = 0.7071752369554196;

        const double K = std::tan(kPi * f0 / sampleRate);
        const double Vh = std::pow(10.0, G / 20.0);
        const double Vb = std::pow(Vh, 0.4996667741545416);

        const double K2 = K * K;
        const double a0 = 1.0 + K / Q + K2;
        const double invA0 = 1.0 / a0;

        b0_ = (Vh + Vb * K / Q + K2) * invA0;
        b1_ = 2.0 * (K2 - Vh) * invA0;
        b2_ = (Vh - Vb * K / Q + K2) * invA0;
        a1_ = 2.0 * (K2 - 1.0) * invA0;
        a2_ = (1.0 - K / Q + K2) * invA0;
    }

    /**
     * ITU-R BS.1770-4 K-weighting Stage 2: RLB high-pass filter (~38.1 Hz).
     * Uses exact analog prototype + bilinear transform for arbitrary sample rates.
     */
    void setKWeightingStage2(double sampleRate) noexcept {
        if (sampleRate <= 0.0) {
            setBypass();
            return;
        }
        constexpr double f0 = 38.13547087602444;
        constexpr double Q = 0.5003270373238773;

        const double K = std::tan(kPi * f0 / sampleRate);
        const double K2 = K * K;
        const double a0 = 1.0 + K / Q + K2;
        const double invA0 = 1.0 / a0;

        b0_ = 1.0 * invA0;
        b1_ = -2.0 * invA0;
        b2_ = 1.0 * invA0;
        a1_ = 2.0 * (K2 - 1.0) * invA0;
        a2_ = (1.0 - K / Q + K2) * invA0;
    }

    inline float processSample(float x) noexcept {
        const double in = static_cast<double>(x);
        const double out = b0_ * in + z1_;
        z1_ = b1_ * in - a1_ * out + z2_;
        z2_ = b2_ * in - a2_ * out;
        return static_cast<float>(out);
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    double b0_ = 1.0, b1_ = 0.0, b2_ = 0.0;
    double a1_ = 0.0, a2_ = 0.0;
    double z1_ = 0.0, z2_ = 0.0;
};

} // namespace autogain
