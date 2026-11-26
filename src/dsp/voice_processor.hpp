#pragma once

#include "biquad.hpp"
#include "rnnoise_wrapper.hpp"
#include <array>
#include <atomic>
#include <vector>
#include <algorithm>
#include <cmath>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <immintrin.h>
#endif

namespace autogain {

enum InputMode : int {
    kInputAuto = 0,
    kInputMonoSum = 1,
    kInputLeftOnly = 2,
    kInputRightOnly = 3,
};

struct VoiceParams {
    bool bypass = false;
    float hpfHz = 80.0f;             // 20 .. 160 Hz
    float nsStrength = 1.0f;         // 0.0 .. 1.0 (0..100%)
    float vadThreshold = 0.50f;      // 0.0 .. 0.95 (0..95%)
    float gateFloorDb = -24.0f;      // -60 .. 0 dB
    bool agcEnable = true;
    float targetLufs = -16.0f;       // -28 .. -10 LUFS
    float agcMaxBoostDb = 18.0f;     // 0 .. 30 dB
    float agcSpeed = 0.50f;          // 0.0 .. 1.0 (0..100%)
    float limiterCeilingDb = -1.0f;  // -6.0 .. -0.1 dBFS
    float outputGainDb = 0.0f;       // -12 .. +12 dB
    int inputMode = kInputAuto;      // 0..3
};

inline void clampVoiceParams(VoiceParams& p) noexcept {
    const VoiceParams def{};
    auto sane = [](float v, float fallback, float lo, float hi) noexcept -> float {
        return std::isfinite(v) ? std::clamp(v, lo, hi) : fallback;
    };
    p.hpfHz            = sane(p.hpfHz,            def.hpfHz,            20.0f,  160.0f);
    p.nsStrength       = sane(p.nsStrength,       def.nsStrength,        0.0f,    1.0f);
    p.vadThreshold     = sane(p.vadThreshold,     def.vadThreshold,      0.0f,   0.95f);
    p.gateFloorDb      = sane(p.gateFloorDb,      def.gateFloorDb,     -60.0f,    0.0f);
    p.targetLufs       = sane(p.targetLufs,       def.targetLufs,      -28.0f,  -10.0f);
    p.agcMaxBoostDb    = sane(p.agcMaxBoostDb,    def.agcMaxBoostDb,     0.0f,   30.0f);
    p.agcSpeed         = sane(p.agcSpeed,         def.agcSpeed,          0.0f,    1.0f);
    p.limiterCeilingDb = sane(p.limiterCeilingDb, def.limiterCeilingDb, -6.0f,   -0.1f);
    p.outputGainDb     = sane(p.outputGainDb,     def.outputGainDb,    -12.0f,   12.0f);
    p.inputMode        = std::clamp(p.inputMode, static_cast<int>(kInputAuto), static_cast<int>(kInputRightOnly));
}

/**
 * Production Broadcast Voice Channel Strip:
 *  0. Input NaN/Inf Sanitizer & Input Routing (Auto / MonoSum / Left / Right)
 *  1. Sub-sonic / Plosive High-Pass Filter (2nd-order Butterworth)
 *  2. Xiph RNNoise Neural Noise Suppression (phase-aligned wet/dry, multi-rate safe, per-sample VAD)
 *  3. Neural VAD-Gated Soft Expander / Gate (2ms atk, 150ms hold, 180ms rel)
 *  4. ITU-R BS.1770-4 K-Weighted Speech-Gated AutoGain (400ms LUFS AGC, freezes during silence)
 *  5. Output Trim + 2ms O(1) Sliding-Window Lookahead Brickwall Limiter (0.4ms atk)
 *  6. Delay-Compensated Smooth Bypass Crossfade
 *
 * Supports Mono->Mono, Mono->Stereo, Stereo->Stereo. Zero heap allocations in process().
 */
class VoiceProcessor {
public:
    static constexpr int kMaxChannels = 2;
    static constexpr int kMaxBlockSize = 8192;
    static constexpr int kMaxLookaheadSamples = 512; // Enough for up to 192 kHz * 2ms
    static constexpr int kMaxBypassDelay = 4096;     // Enough for up to 192 kHz (~21ms)

    VoiceProcessor() = default;

    void prepare(double sampleRate, int numChannels = 1) {
        (void)numChannels;
        sampleRate_ = std::max(sampleRate, 8000.0);

        lookaheadSamples_ = std::clamp(static_cast<int>(std::llround(sampleRate_ * 0.002)), 16, kMaxLookaheadSamples - 1);

        for (int ch = 0; ch < kMaxChannels; ++ch) {
            work_[ch].assign(kMaxBlockSize, 0.0f);
            dryBypassDelay_[ch].assign(kMaxBypassDelay, 0.0f);
            rnnoise_[ch].init(sampleRate_);
        }
        vadBuf_.assign(kMaxBlockSize, 0.0f);
        vadTmp_.assign(kMaxBlockSize, 0.0f);

        updateFilters();
        reset();
    }

    void reset() noexcept {
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            hpf_[ch].reset();
            kStage1_[ch].reset();
            kStage2_[ch].reset();
            rnnoise_[ch].reset();
            limDelay_[ch].fill(0.0f);
            std::fill(dryBypassDelay_[ch].begin(), dryBypassDelay_[ch].end(), 0.0f);
        }
        limPeakRing_.fill(0.0f);
        limDequeIdx_.fill(0);
        limDequeHead_ = 0;
        limDequeTail_ = 0;
        limSampleCounter_ = 0;
        limWriteIdx_ = 0;
        limPeakEnv_ = 0.0f;

        bypassWriteIdx_ = 0;

        vadSmooth_ = 0.0f;
        gateHoldCounter_ = 0;
        gateGain_ = 1.0f;
        lufsMeanSq_ = 1e-4; // -40 LUFS neutral starting point
        agcGainDb_ = 0.0;
        bypassMix_ = params_.bypass ? 0.0f : 1.0f;

        smoothNsStrength_ = params_.nsStrength;
        smoothGateFloorLin_ = std::pow(10.0f, params_.gateFloorDb / 20.0f);
        smoothLimCeilingLin_ = std::pow(10.0f, params_.limiterCeilingDb / 20.0f);
        smoothOutTrimLin_ = std::pow(10.0f, params_.outputGainDb / 20.0f);

        telInPeakDb_.store(-120.0f, std::memory_order_relaxed);
        telOutPeakDb_.store(-120.0f, std::memory_order_relaxed);
        telAgcGainDb_.store(0.0f, std::memory_order_relaxed);
        telGateGain_.store(1.0f, std::memory_order_relaxed);
    }

    void setParams(const VoiceParams& p) noexcept {
        VoiceParams clamped = p;
        clampVoiceParams(clamped);
        const bool hpfChanged = std::abs(clamped.hpfHz - params_.hpfHz) > 0.01f;
        params_ = clamped;
        if (hpfChanged && sampleRate_ > 0.0) {
            for (int ch = 0; ch < kMaxChannels; ++ch) {
                hpf_[ch].setHighPass(sampleRate_, params_.hpfHz, 0.7071067811865475);
            }
        }
    }

    const VoiceParams& params() const noexcept { return params_; }

    int getLatencySamples() const noexcept {
        return rnnoise_[0].latencySamples() + lookaheadSamples_;
    }

    void process(float* const* inputs, float* const* outputs, int numChannels, int numSamples) noexcept {
        process(inputs, outputs, numChannels, numChannels, numSamples);
    }

    void process(float* const* inputs, float* const* outputs, int inChannels, int outChannels, int numSamples) noexcept {
        if (numSamples <= 0 || inChannels <= 0 || outChannels <= 0 || !inputs || !outputs) return;

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        const unsigned int oldMxcsr = _mm_getcsr();
        _mm_setcsr(oldMxcsr | 0x8040u); // FTZ + DAZ: zero denormal overhead
#endif

        const int inCh = std::clamp(inChannels, 1, kMaxChannels);
        const int outCh = std::clamp(outChannels, 1, kMaxChannels);

        float blockInPeak = 0.0f;
        float blockOutPeak = 0.0f;

        int offset = 0;
        while (offset < numSamples) {
            const int chunk = std::min(numSamples - offset, kMaxBlockSize);
            processChunk(inputs, outputs, inCh, outCh, offset, chunk, blockInPeak, blockOutPeak);
            offset += chunk;
        }

        const float inDb = (blockInPeak > 1e-6f) ? std::max(-120.0f, 20.0f * std::log10(blockInPeak)) : -120.0f;
        const float outDb = (blockOutPeak > 1e-6f) ? std::max(-120.0f, 20.0f * std::log10(blockOutPeak)) : -120.0f;
        telInPeakDb_.store(inDb, std::memory_order_relaxed);
        telOutPeakDb_.store(outDb, std::memory_order_relaxed);
        telAgcGainDb_.store(static_cast<float>(agcGainDb_), std::memory_order_relaxed);
        telGateGain_.store(gateGain_, std::memory_order_relaxed);

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
        _mm_setcsr(oldMxcsr);
#endif
    }

    // Telemetry getters (real-time safe)
    double currentAgcGainDb() const noexcept { return agcGainDb_; }
    float currentGateGain() const noexcept { return gateGain_; }
    float currentVad() const noexcept { return vadSmooth_; }
    float telemetryInDb() const noexcept { return telInPeakDb_.load(std::memory_order_relaxed); }
    float telemetryOutDb() const noexcept { return telOutPeakDb_.load(std::memory_order_relaxed); }
    float telemetryAgcDb() const noexcept { return telAgcGainDb_.load(std::memory_order_relaxed); }
    float telemetryGateGain() const noexcept { return telGateGain_.load(std::memory_order_relaxed); }

private:
    void updateFilters() noexcept {
        for (int ch = 0; ch < kMaxChannels; ++ch) {
            hpf_[ch].setHighPass(sampleRate_, params_.hpfHz, 0.7071067811865475);
            kStage1_[ch].setKWeightingStage1(sampleRate_);
            kStage2_[ch].setKWeightingStage2(sampleRate_);
        }
    }

    static inline float sanitizeSample(float raw) noexcept {
        return std::isfinite(raw) ? std::clamp(raw, -16.0f, 16.0f) : 0.0f;
    }

    void processChunk(float* const* inputs, float* const* outputs, int inCh, int outCh,
                      int offset, int n, float& blockInPeak, float& blockOutPeak) noexcept {
        // Stage 0: Sanitize input (NaN/Inf guard), apply InputMode routing, and feed delay-compensated dry buffer
        const bool collapseToMono = (inCh == 1) || (params_.inputMode != kInputAuto);
        const int dspCh = collapseToMono ? 1 : 2;
        const int totalLatency = std::min(getLatencySamples(), kMaxBypassDelay - 1);

        if (collapseToMono) {
            float* w0 = work_[0].data();
            const float* src0 = inputs[0] + offset;
            const float* src1 = (inCh >= 2 && inputs[1]) ? (inputs[1] + offset) : src0;
            for (int i = 0; i < n; ++i) {
                const float c0 = sanitizeSample(src0[i]);
                const float c1 = sanitizeSample(src1[i]);
                float routed = c0;
                if (inCh >= 2) {
                    switch (params_.inputMode) {
                        case kInputMonoSum:   routed = 0.5f * (c0 + c1); break;
                        case kInputRightOnly: routed = c1; break;
                        case kInputLeftOnly:
                        case kInputAuto:
                        default:              routed = c0; break;
                    }
                }
                w0[i] = routed;
                blockInPeak = std::max(blockInPeak, std::abs(routed));
            }
        } else {
            for (int ch = 0; ch < dspCh; ++ch) {
                const float* src = inputs[ch] + offset;
                float* w = work_[ch].data();
                for (int i = 0; i < n; ++i) {
                    const float clean = sanitizeSample(src[i]);
                    w[i] = clean;
                    blockInPeak = std::max(blockInPeak, std::abs(clean));
                }
            }
        }

        // Smooth nsStrength once per chunk for RNNoise wet/dry mix
        const double invFs = 1.0 / sampleRate_;
        const float smoothCoeff = 1.0f - std::exp(static_cast<float>(-invFs / 0.008));
        smoothNsStrength_ += (1.0f - std::exp(static_cast<float>(-static_cast<double>(n) * invFs / 0.008))) *
                             (params_.nsStrength - smoothNsStrength_);

        // Stage 1: Sub-Sonic / Plosive HPF
        for (int ch = 0; ch < dspCh; ++ch) {
            float* buf = work_[ch].data();
            for (int i = 0; i < n; ++i) {
                const float dryVal = std::clamp(buf[i], -1.0f, 1.0f);
                const int dWrite = (bypassWriteIdx_ + i) % kMaxBypassDelay;
                dryBypassDelay_[ch][dWrite] = dryVal;
                buf[i] = hpf_[ch].processSample(buf[i]);
            }
        }

        // Stage 2: Xiph RNNoise Neural Noise Suppression with per-sample VAD output
        rnnoise_[0].processBlock(work_[0].data(), work_[0].data(), n, smoothNsStrength_, vadBuf_.data());
        if (dspCh == 2) {
            rnnoise_[1].processBlock(work_[1].data(), work_[1].data(), n, smoothNsStrength_, vadTmp_.data());
            for (int i = 0; i < n; ++i) {
                vadBuf_[i] = std::max(vadBuf_[i], vadTmp_[i]);
            }
        }

        // Precompute per-sample time constants
        const float vadAtk = 1.0f - std::exp(static_cast<float>(-invFs / 0.004));
        const float vadRel = 1.0f - std::exp(static_cast<float>(-invFs / 0.060));
        const float gateAtk = 1.0f - std::exp(static_cast<float>(-invFs / 0.002));
        const float gateRel = 1.0f - std::exp(static_cast<float>(-invFs / 0.180));
        const int holdSamples = static_cast<int>(sampleRate_ * 0.150);
        const float targetGateFloorLin = std::pow(10.0f, params_.gateFloorDb / 20.0f);

        // ITU-R BS.1770-4 400 ms momentary integration coefficient
        const double lufsCoeff = 1.0 - std::exp(-invFs / 0.400);

        // AGC slew rates (dB per sample) scaled by agcSpeed [0..1]
        const double speedScale = 0.35 + 1.65 * static_cast<double>(params_.agcSpeed);
        const double agcUpRatePerSample = (6.0 * speedScale) * invFs;
        const double agcDownRatePerSample = (12.0 * speedScale) * invFs;

        // Limiter constants (0.4ms smooth attack so 2ms lookahead catches >99% before peak arrives + 60ms release)
        const float targetLimCeilingLin = std::pow(10.0f, params_.limiterCeilingDb / 20.0f);
        const float limAtkCoeff = std::exp(static_cast<float>(-invFs / 0.0004));
        const float limRelCoeff = std::exp(static_cast<float>(-invFs / 0.060));
        const float targetOutTrimLin = std::pow(10.0f, params_.outputGainDb / 20.0f);
        const float bypassStep = static_cast<float>(invFs / 0.015);

        for (int i = 0; i < n; ++i) {
            // Parameter smoothing (anti-zipper)
            smoothGateFloorLin_ += smoothCoeff * (targetGateFloorLin - smoothGateFloorLin_);
            smoothLimCeilingLin_ += smoothCoeff * (targetLimCeilingLin - smoothLimCeilingLin_);
            smoothOutTrimLin_ += smoothCoeff * (targetOutTrimLin - smoothOutTrimLin_);

            // Smooth per-sample neural VAD
            const float vadTarget = vadBuf_[i];
            vadSmooth_ += (vadTarget > vadSmooth_ ? vadAtk : vadRel) * (vadTarget - vadSmooth_);

            // Stage 3: Neural VAD-Gated Soft Expander / Gate
            const bool speechDetected = (vadSmooth_ >= params_.vadThreshold) || (params_.vadThreshold <= 0.001f);
            if (speechDetected) {
                gateHoldCounter_ = holdSamples;
            } else if (gateHoldCounter_ > 0) {
                --gateHoldCounter_;
            }
            const float gateTarget = (gateHoldCounter_ > 0) ? 1.0f : smoothGateFloorLin_;
            gateGain_ += (gateTarget > gateGain_ ? gateAtk : gateRel) * (gateTarget - gateGain_);

            for (int ch = 0; ch < dspCh; ++ch) {
                work_[ch][i] *= gateGain_;
            }

            // Stage 4: ITU-R BS.1770-4 K-Weighted Speech-Gated AutoGain
            double kEnergy = 0.0;
            for (int ch = 0; ch < dspCh; ++ch) {
                const float k1 = kStage1_[ch].processSample(work_[ch][i]);
                const float k2 = kStage2_[ch].processSample(k1);
                kEnergy += static_cast<double>(k2) * static_cast<double>(k2);
            }
            if (dspCh == 2) {
                kEnergy *= 0.5; // Prevent dual-mono +3.01 dB loudness bias vs mono
            }

            // Speech-gated LUFS integrator: supports quiet dynamic mics down to -68 dB / -65 LUFS
            if (speechDetected && kEnergy > 1.6e-7) {
                lufsMeanSq_ = (1.0 - lufsCoeff) * lufsMeanSq_ + lufsCoeff * kEnergy;
            }
            const double currentLufs = -0.691 + 10.0 * std::log10(std::max(lufsMeanSq_, 1e-12));

            if (params_.agcEnable) {
                if (speechDetected && kEnergy > 1.6e-7 && currentLufs > -65.0) {
                    const double desiredGainDb = std::clamp(
                        static_cast<double>(params_.targetLufs) - currentLufs,
                        -18.0,
                        static_cast<double>(params_.agcMaxBoostDb)
                    );
                    const double diff = desiredGainDb - agcGainDb_;
                    if (diff > 0.0) {
                        agcGainDb_ += std::min(diff, agcUpRatePerSample);
                    } else {
                        agcGainDb_ += std::max(diff, -agcDownRatePerSample);
                    }
                }
            } else {
                if (std::abs(agcGainDb_) > 1e-4) {
                    const double step = agcDownRatePerSample;
                    if (agcGainDb_ > 0.0) agcGainDb_ = std::max(0.0, agcGainDb_ - step);
                    else agcGainDb_ = std::min(0.0, agcGainDb_ + step);
                } else {
                    agcGainDb_ = 0.0;
                }
            }

            // Stage 5: Apply AGC + Output Trim BEFORE O(1) 2 ms Sliding-Window Lookahead Limiter
            const float agcAndTrimLin = static_cast<float>(std::pow(10.0, agcGainDb_ / 20.0)) * smoothOutTrimLin_;
            float samplePeak = 0.0f;
            for (int ch = 0; ch < dspCh; ++ch) {
                work_[ch][i] *= agcAndTrimLin;
                samplePeak = std::max(samplePeak, std::abs(work_[ch][i]));
            }

            // O(1) monotonic deque sliding-window max over lookaheadSamples_
            limPeakRing_[limWriteIdx_] = samplePeak;
            while (limDequeHead_ != limDequeTail_) {
                const int backIdx = (limDequeTail_ - 1 + kMaxLookaheadSamples) % kMaxLookaheadSamples;
                const int backSampleId = limDequeIdx_[backIdx];
                const int backRingPos = backSampleId % kMaxLookaheadSamples;
                if (limPeakRing_[backRingPos] <= samplePeak) {
                    limDequeTail_ = backIdx;
                } else {
                    break;
                }
            }
            limDequeIdx_[limDequeTail_] = limSampleCounter_;
            limDequeTail_ = (limDequeTail_ + 1) % kMaxLookaheadSamples;
            while (limDequeHead_ != limDequeTail_ &&
                   (limSampleCounter_ - limDequeIdx_[limDequeHead_]) >= lookaheadSamples_) {
                limDequeHead_ = (limDequeHead_ + 1) % kMaxLookaheadSamples;
            }
            const float windowMax = (limDequeHead_ != limDequeTail_)
                ? limPeakRing_[limDequeIdx_[limDequeHead_] % kMaxLookaheadSamples]
                : samplePeak;
            ++limSampleCounter_;

            if (windowMax > limPeakEnv_) {
                limPeakEnv_ = limAtkCoeff * limPeakEnv_ + (1.0f - limAtkCoeff) * windowMax;
            } else {
                limPeakEnv_ = limRelCoeff * limPeakEnv_ + (1.0f - limRelCoeff) * windowMax;
            }
            const float limGain = (limPeakEnv_ > smoothLimCeilingLin_) ? (smoothLimCeilingLin_ / limPeakEnv_) : 1.0f;

            const int readIdx = (limWriteIdx_ - lookaheadSamples_ + kMaxLookaheadSamples) % kMaxLookaheadSamples;
            const int dRead = (bypassWriteIdx_ + i - totalLatency + kMaxBypassDelay) % kMaxBypassDelay;

            // Stage 6: Delay-compensated smooth bypass crossfade + brickwall safety clamp
            const float targetActive = params_.bypass ? 0.0f : 1.0f;
            if (bypassMix_ < targetActive) bypassMix_ = std::min(targetActive, bypassMix_ + bypassStep);
            else if (bypassMix_ > targetActive) bypassMix_ = std::max(targetActive, bypassMix_ - bypassStep);

            for (int ch = 0; ch < dspCh; ++ch) {
                limDelay_[ch][limWriteIdx_] = work_[ch][i];
                const float delayed = limDelay_[ch][readIdx];
                const float wetOut = std::clamp(delayed * limGain, -smoothLimCeilingLin_, smoothLimCeilingLin_);
                const float dryDelayed = dryBypassDelay_[ch][dRead];
                const float finalSample = std::clamp(wetOut * bypassMix_ + dryDelayed * (1.0f - bypassMix_), -1.0f, 1.0f);
                outputs[ch][offset + i] = finalSample;
                blockOutPeak = std::max(blockOutPeak, std::abs(finalSample));
            }
            if (dspCh == 1 && outCh == 2) {
                const float monoOut = outputs[0][offset + i];
                outputs[1][offset + i] = monoOut;
            }

            limWriteIdx_ = (limWriteIdx_ + 1) % kMaxLookaheadSamples;
        }
        bypassWriteIdx_ = (bypassWriteIdx_ + n) % kMaxBypassDelay;
    }

    VoiceParams params_{};
    double sampleRate_ = 48000.0;
    int lookaheadSamples_ = 96;

    std::array<Biquad, kMaxChannels> hpf_;
    std::array<Biquad, kMaxChannels> kStage1_;
    std::array<Biquad, kMaxChannels> kStage2_;
    std::array<RnnoiseChannel, kMaxChannels> rnnoise_;

    std::array<std::vector<float>, kMaxChannels> work_;
    std::array<std::vector<float>, kMaxChannels> dryBypassDelay_;
    std::vector<float> vadBuf_;
    std::vector<float> vadTmp_;
    int bypassWriteIdx_ = 0;

    std::array<std::array<float, kMaxLookaheadSamples>, kMaxChannels> limDelay_{};
    std::array<float, kMaxLookaheadSamples> limPeakRing_{};
    std::array<int, kMaxLookaheadSamples> limDequeIdx_{};
    int limDequeHead_ = 0;
    int limDequeTail_ = 0;
    int limSampleCounter_ = 0;
    int limWriteIdx_ = 0;
    float limPeakEnv_ = 0.0f;

    float vadSmooth_ = 0.0f;
    int gateHoldCounter_ = 0;
    float gateGain_ = 1.0f;

    double lufsMeanSq_ = 1e-4;
    double agcGainDb_ = 0.0;
    float bypassMix_ = 1.0f;

    float smoothNsStrength_ = 1.0f;
    float smoothGateFloorLin_ = 0.0630957f;
    float smoothLimCeilingLin_ = 0.891251f;
    float smoothOutTrimLin_ = 1.0f;

    std::atomic<float> telInPeakDb_{-120.0f};
    std::atomic<float> telOutPeakDb_{-120.0f};
    std::atomic<float> telAgcGainDb_{0.0f};
    std::atomic<float> telGateGain_{1.0f};
};

} // namespace autogain
