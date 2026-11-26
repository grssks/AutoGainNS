#pragma once

#include "biquad.hpp"
#include "rnnoise.h"
#include <array>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace autogain {

/**
 * Real-time safe wrapper around Xiph RNNoise (480-sample frames, 48 kHz native).
 * - Zero-resampling fast path when host sample rate is ~48 kHz.
 * - Allocation-free anti-aliased linear resampler for non-48 kHz rates (e.g. 16 / 44.1 / 96 / 192 kHz).
 * - Phase-aligned dry delay line for artifact-free Wet/Dry mixing.
 * - Per-sample VAD probability output to prevent block-size gate quantization.
 */
class RnnoiseChannel {
public:
    static constexpr int kFrameSize = 480;
    static constexpr int kRingCap = 16384; // Power of 2, handles up to 192 kHz + large blocks
    static constexpr int kRingMask = kRingCap - 1;
    static constexpr double kNativeRate = 48000.0;

    RnnoiseChannel() = default;
    ~RnnoiseChannel() { destroy(); }

    RnnoiseChannel(const RnnoiseChannel&) = delete;
    RnnoiseChannel& operator=(const RnnoiseChannel&) = delete;

    void init(double sampleRate = 48000.0) {
        sampleRate_ = std::max(sampleRate, 8000.0);
        native48k_ = (std::abs(sampleRate_ - kNativeRate) < 100.0);
        dryDelaySamples_ = native48k_
            ? kFrameSize
            : std::clamp(static_cast<int>(std::llround(kFrameSize * (sampleRate_ / kNativeRate))), 80, kRingCap / 2);

        if (!native48k_) {
            if (sampleRate_ > 49000.0) {
                aaInLpf_.setLowPass(sampleRate_, 20000.0);
                aaOutLpf_.setLowPass(sampleRate_, 20000.0);
            } else {
                aaInLpf_.setBypass();
                aaOutLpf_.setLowPass(sampleRate_, sampleRate_ * 0.44);
            }
        } else {
            aaInLpf_.setBypass();
            aaOutLpf_.setBypass();
        }

        if (inBuf_.empty()) {
            inBuf_.assign(kRingCap, 0.0f);
            outBuf_.assign(kRingCap, 0.0f);
            dryBuf_.assign(kRingCap, 0.0f);
        }
        if (!state_) {
            state_ = rnnoise_create(nullptr);
        } else {
            rnnoise_init(state_, nullptr);
        }
        reset();
    }

    void destroy() noexcept {
        if (state_) {
            rnnoise_destroy(state_);
            state_ = nullptr;
        }
    }

    void reset() noexcept {
        if (state_) {
            rnnoise_init(state_, nullptr);
        }
        aaInLpf_.reset();
        aaOutLpf_.reset();
        inHead_ = inTail_ = inCount_ = 0;
        outHead_ = outTail_ = outCount_ = 0;
        dryHead_ = dryTail_ = dryCount_ = 0;
        std::fill(inBuf_.begin(), inBuf_.end(), 0.0f);
        std::fill(outBuf_.begin(), outBuf_.end(), 0.0f);
        std::fill(dryBuf_.begin(), dryBuf_.end(), 0.0f);

        // Pre-fill outBuf_ in 48 kHz domain (kFrameSize) and dryBuf_ in host-rate domain (dryDelaySamples_)
        for (int i = 0; i < kFrameSize; ++i) {
            pushOut(0.0f);
        }
        for (int i = 0; i < dryDelaySamples_; ++i) {
            pushDry(0.0f);
        }
        inPhase_ = 0.0;
        inPrev_ = 0.0f;
        outPhase_ = 0.0;
        outPrev_ = 0.0f;
        outCurr_ = 0.0f;
        lastVad_ = 0.0f;
    }

    int latencySamples() const noexcept {
        return dryDelaySamples_;
    }

    /**
     * Process numSamples in-place (or from inPtr to outPtr).
     * Optionally writes per-sample VAD probability [0.0, 1.0] into vadOut[0..numSamples-1].
     * Returns the latest RNNoise neural VAD probability [0.0, 1.0].
     */
    float processBlock(const float* inPtr, float* outPtr, int numSamples, float wetMix, float* vadOut = nullptr) noexcept {
        if (!state_) {
            if (inPtr != outPtr) {
                std::memcpy(outPtr, inPtr, static_cast<size_t>(numSamples) * sizeof(float));
            }
            if (vadOut) {
                for (int i = 0; i < numSamples; ++i) vadOut[i] = 1.0f;
            }
            return 1.0f;
        }

        const float mix = std::clamp(wetMix, 0.0f, 1.0f);
        const float dryMix = 1.0f - mix;

        if (native48k_) {
            for (int i = 0; i < numSamples; ++i) {
                const float s = inPtr[i];
                pushIn(s);
                pushDry(s);

                if (inCount_ >= kFrameSize) {
                    runRnnoiseFrame();
                }

                const float wetSample = (outCount_ > 0) ? popOut() : 0.0f;
                const float drySample = (dryCount_ > 0) ? popDry() : 0.0f;
                outPtr[i] = wetSample * mix + drySample * dryMix;
                if (vadOut) vadOut[i] = lastVad_;
            }
            return lastVad_;
        }

        // Resampled path for non-48 kHz host rates (e.g. 16 kHz, 44.1 kHz, 96 kHz, 192 kHz)
        const double inStep = kNativeRate / sampleRate_;  // 48k samples produced per host sample
        const double outStep = sampleRate_ / kNativeRate; // host samples per 48k sample
        const double invInStep = 1.0 / inStep;
        const double invOutStep = 1.0 / outStep;

        for (int i = 0; i < numSamples; ++i) {
            const float raw = inPtr[i];
            pushDry(raw);
            const float s = aaInLpf_.processSample(raw);

            // Upsample/downsample host -> 48 kHz into inBuf_
            inPhase_ += inStep;
            while (inPhase_ >= 1.0) {
                inPhase_ -= 1.0;
                const float t = static_cast<float>(1.0 - inPhase_ * invInStep);
                const float interp = inPrev_ + std::clamp(t, 0.0f, 1.0f) * (s - inPrev_);
                pushIn(interp);
                if (inCount_ >= kFrameSize) {
                    runRnnoiseFrame();
                }
            }
            inPrev_ = s;

            // Resample 48 kHz outBuf_ -> host sample rate
            while (outPhase_ < 1.0 && outCount_ > 0) {
                outPrev_ = outCurr_;
                outCurr_ = popOut();
                outPhase_ += outStep;
            }
            float wetSample = outCurr_;
            if (outStep > 0.0 && outPhase_ >= 1.0) {
                const float frac = static_cast<float>(std::clamp(1.0 - (outPhase_ - 1.0) * invOutStep, 0.0, 1.0));
                wetSample = outPrev_ + frac * (outCurr_ - outPrev_);
                outPhase_ -= 1.0;
            }
            wetSample = aaOutLpf_.processSample(wetSample);

            const float drySample = (dryCount_ > 0) ? popDry() : 0.0f;
            outPtr[i] = wetSample * mix + drySample * dryMix;
            if (vadOut) vadOut[i] = lastVad_;
        }
        return lastVad_;
    }

    float lastVad() const noexcept { return lastVad_; }

private:
    void runRnnoiseFrame() noexcept {
        std::array<float, kFrameSize> frameIn{};
        std::array<float, kFrameSize> frameOut{};
        for (int k = 0; k < kFrameSize; ++k) {
            frameIn[k] = popIn() * 32768.0f;
        }
        lastVad_ = rnnoise_process_frame(state_, frameOut.data(), frameIn.data());
        for (int k = 0; k < kFrameSize; ++k) {
            pushOut(frameOut[k] * (1.0f / 32768.0f));
        }
    }

    inline void pushIn(float v) noexcept {
        if (inCount_ < kRingCap) {
            inBuf_[inTail_] = v;
            inTail_ = (inTail_ + 1) & kRingMask;
            ++inCount_;
        }
    }
    inline float popIn() noexcept {
        const float v = inBuf_[inHead_];
        inHead_ = (inHead_ + 1) & kRingMask;
        --inCount_;
        return v;
    }

    inline void pushOut(float v) noexcept {
        if (outCount_ < kRingCap) {
            outBuf_[outTail_] = v;
            outTail_ = (outTail_ + 1) & kRingMask;
            ++outCount_;
        }
    }
    inline float popOut() noexcept {
        const float v = outBuf_[outHead_];
        outHead_ = (outHead_ + 1) & kRingMask;
        --outCount_;
        return v;
    }

    inline void pushDry(float v) noexcept {
        if (dryCount_ < kRingCap) {
            dryBuf_[dryTail_] = v;
            dryTail_ = (dryTail_ + 1) & kRingMask;
            ++dryCount_;
        }
    }
    inline float popDry() noexcept {
        const float v = dryBuf_[dryHead_];
        dryHead_ = (dryHead_ + 1) & kRingMask;
        --dryCount_;
        return v;
    }

    DenoiseState* state_ = nullptr;
    double sampleRate_ = 48000.0;
    bool native48k_ = true;
    int dryDelaySamples_ = kFrameSize;

    Biquad aaInLpf_{};
    Biquad aaOutLpf_{};

    std::vector<float> inBuf_;
    std::vector<float> outBuf_;
    std::vector<float> dryBuf_;
    int inHead_ = 0, inTail_ = 0, inCount_ = 0;
    int outHead_ = 0, outTail_ = 0, outCount_ = 0;
    int dryHead_ = 0, dryTail_ = 0, dryCount_ = 0;

    double inPhase_ = 0.0;
    float inPrev_ = 0.0f;
    double outPhase_ = 0.0;
    float outPrev_ = 0.0f;
    float outCurr_ = 0.0f;
    float lastVad_ = 0.0f;
};

} // namespace autogain
