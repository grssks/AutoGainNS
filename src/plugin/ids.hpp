#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "dsp/voice_processor.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace autogain {

// Unique FUIDs for AutoGainNS Processor & Controller
static const Steinberg::FUID kProcessorUID(0x4A7E91B2, 0x8C3D4F10, 0xA2B511E9, 0x90261125);
static const Steinberg::FUID kControllerUID(0x7C2D54E8, 0x19FA4B21, 0xB8C300D7, 0x90261126);

enum ParamID : Steinberg::Vst::ParamID {
    kParamBypass = 0,
    kParamHpfHz,
    kParamNsStrength,
    kParamVadThreshold,
    kParamGateFloorDb,
    kParamAgcEnable,
    kParamTargetLufs,
    kParamAgcMaxBoostDb,
    kParamAgcSpeed,
    kParamLimiterCeilingDb,
    kParamOutputGainDb,
    kParamInputMode,
    kNumParams
};

inline double normToRange(double norm, double minVal, double maxVal) noexcept {
    return minVal + std::clamp(norm, 0.0, 1.0) * (maxVal - minVal);
}

inline double rangeToNorm(double val, double minVal, double maxVal) noexcept {
    return std::clamp((val - minVal) / (maxVal - minVal), 0.0, 1.0);
}

inline void applyParamNormalized(VoiceParams& p, Steinberg::Vst::ParamID id, double v) noexcept {
    switch (id) {
        case kParamBypass:           p.bypass = (v >= 0.5); break;
        case kParamHpfHz:            p.hpfHz = static_cast<float>(normToRange(v, 20.0, 160.0)); break;
        case kParamNsStrength:       p.nsStrength = static_cast<float>(std::clamp(v, 0.0, 1.0)); break;
        case kParamVadThreshold:     p.vadThreshold = static_cast<float>(normToRange(v, 0.0, 0.95)); break;
        case kParamGateFloorDb:      p.gateFloorDb = static_cast<float>(normToRange(v, -60.0, 0.0)); break;
        case kParamAgcEnable:        p.agcEnable = (v >= 0.5); break;
        case kParamTargetLufs:       p.targetLufs = static_cast<float>(normToRange(v, -28.0, -10.0)); break;
        case kParamAgcMaxBoostDb:    p.agcMaxBoostDb = static_cast<float>(normToRange(v, 0.0, 30.0)); break;
        case kParamAgcSpeed:         p.agcSpeed = static_cast<float>(std::clamp(v, 0.0, 1.0)); break;
        case kParamLimiterCeilingDb: p.limiterCeilingDb = static_cast<float>(normToRange(v, -6.0, -0.1)); break;
        case kParamOutputGainDb:     p.outputGainDb = static_cast<float>(normToRange(v, -12.0, 12.0)); break;
        case kParamInputMode:        p.inputMode = std::clamp(static_cast<int>(std::llround(std::clamp(v, 0.0, 1.0) * 3.0)), 0, 3); break;
        default: break;
    }
}

struct PresetSpec {
    const wchar_t* name;
    const wchar_t* tip;
    double hpfHz;
    double nsStrength;
    double vadThreshold;
    double gateFloorDb;
    double agcEnable;
    double targetLufs;
    double agcMaxBoostDb;
    double agcSpeed;
    double limiterCeilingDb;
    double outputGainDb;
};

inline constexpr int kNumPresets = 4;
inline constexpr PresetSpec kPresets[kNumPresets] = {
    {
        L"Balanced",
        L"Balanced: General-purpose defaults (-16 LUFS, -24 dB silence floor)",
        80.0, 1.00, 0.50, -24.0, 1.0, -16.0, 18.0, 0.50, -1.0, 0.0
    },
    {
        L"Discord",
        L"Discord: Tight gate (-30 dB) & full AI noise removal for voice chat",
        90.0, 1.00, 0.60, -30.0, 1.0, -15.0, 18.0, 0.65, -1.0, 0.0
    },
    {
        L"Podcast",
        L"Podcast: Gentle gate (-18 dB) & smoother leveling for natural voice",
        75.0, 0.85, 0.40, -18.0, 1.0, -16.0, 14.0, 0.40, -1.0, 0.0
    },
    {
        L"Stream",
        L"Stream: Louder -13 LUFS target & faster leveling",
        85.0, 0.95, 0.50, -26.0, 1.0, -13.0, 24.0, 0.75, -0.5, 0.0
    },
};

struct StateBlob {
    Steinberg::uint32 magic = 0x41474E53u; // 'AGNS'
    Steinberg::uint32 version = 1;
    Steinberg::uint32 flags = 0;           // bit 0: bypass, bit 1: agcEnable
    Steinberg::int32 inputMode = 0;
    float values[10]{};                    // hpfHz, nsStrength, vadThreshold, gateFloorDb, targetLufs, agcMaxBoostDb, agcSpeed, limiterCeilingDb, outputGainDb, reserved
};

inline Steinberg::tresult writeState(Steinberg::IBStream* state, const VoiceParams& p) {
    if (!state) return Steinberg::kResultFalse;
    VoiceParams clean = p;
    clampVoiceParams(clean);
    StateBlob blob{};
    blob.flags = (clean.bypass ? 1u : 0u) | (clean.agcEnable ? 2u : 0u);
    blob.inputMode = clean.inputMode;
    blob.values[0] = clean.hpfHz;
    blob.values[1] = clean.nsStrength;
    blob.values[2] = clean.vadThreshold;
    blob.values[3] = clean.gateFloorDb;
    blob.values[4] = clean.targetLufs;
    blob.values[5] = clean.agcMaxBoostDb;
    blob.values[6] = clean.agcSpeed;
    blob.values[7] = clean.limiterCeilingDb;
    blob.values[8] = clean.outputGainDb;
    blob.values[9] = 0.0f;
    Steinberg::int32 numWritten = 0;
    return state->write(&blob, sizeof(StateBlob), &numWritten);
}

inline Steinberg::tresult readState(Steinberg::IBStream* state, VoiceParams& out) {
    if (!state) return Steinberg::kResultFalse;
    StateBlob blob{};
    Steinberg::int32 numRead = 0;
    if (state->read(&blob, sizeof(StateBlob), &numRead) != Steinberg::kResultOk || numRead != sizeof(StateBlob)) {
        return Steinberg::kResultFalse;
    }
    if (blob.magic != 0x41474E53u || blob.version != 1) {
        return Steinberg::kResultFalse;
    }
    out.bypass           = (blob.flags & 1u) != 0;
    out.agcEnable        = (blob.flags & 2u) != 0;
    out.inputMode        = blob.inputMode;
    out.hpfHz            = blob.values[0];
    out.nsStrength       = blob.values[1];
    out.vadThreshold     = blob.values[2];
    out.gateFloorDb      = blob.values[3];
    out.targetLufs       = blob.values[4];
    out.agcMaxBoostDb    = blob.values[5];
    out.agcSpeed         = blob.values[6];
    out.limiterCeilingDb = blob.values[7];
    out.outputGainDb     = blob.values[8];
    clampVoiceParams(out);
    return Steinberg::kResultOk;
}

} // namespace autogain
