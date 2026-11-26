#include "plugin/controller.hpp"
#include "plugin/editor_view.hpp"
#include "pluginterfaces/base/ibstream.h"
#include <algorithm>
#include <cstring>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace autogain {

tresult PLUGIN_API AutoGainNSController::initialize(FUnknown* context) {
    tresult res = EditControllerEx1::initialize(context);
    if (res != kResultOk) return res;

    parameters.addParameter(
        STR16("Bypass"), nullptr, 1, 0.0,
        ParameterInfo::kCanAutomate | ParameterInfo::kIsBypass, kParamBypass);

    parameters.addParameter(new RangeParameter(
        STR16("Low-Cut Rumble Filter"), kParamHpfHz, STR16("Hz"),
        20.0, 160.0, 80.0, 0, ParameterInfo::kCanAutomate));

    parameters.addParameter(new RangeParameter(
        STR16("AI Noise Removal"), kParamNsStrength, STR16("%"),
        0.0, 100.0, 100.0, 0, ParameterInfo::kCanAutomate));

    parameters.addParameter(new RangeParameter(
        STR16("Voice Gate Sensitivity"), kParamVadThreshold, STR16("%"),
        0.0, 95.0, 50.0, 0, ParameterInfo::kCanAutomate));

    parameters.addParameter(new RangeParameter(
        STR16("Silence Reduction"), kParamGateFloorDb, STR16("dB"),
        -60.0, 0.0, -24.0, 0, ParameterInfo::kCanAutomate));

    parameters.addParameter(
        STR16("Auto Volume"), nullptr, 1, 1.0,
        ParameterInfo::kCanAutomate, kParamAgcEnable);

    parameters.addParameter(new RangeParameter(
        STR16("Target Voice Volume"), kParamTargetLufs, STR16("LUFS"),
        -28.0, -10.0, -16.0, 0, ParameterInfo::kCanAutomate));

    parameters.addParameter(new RangeParameter(
        STR16("Max Volume Boost"), kParamAgcMaxBoostDb, STR16("dB"),
        0.0, 30.0, 18.0, 0, ParameterInfo::kCanAutomate));

    parameters.addParameter(new RangeParameter(
        STR16("Leveling Speed"), kParamAgcSpeed, STR16("%"),
        0.0, 100.0, 50.0, 0, ParameterInfo::kCanAutomate));

    parameters.addParameter(new RangeParameter(
        STR16("Clip Protection"), kParamLimiterCeilingDb, STR16("dBFS"),
        -6.0, -0.1, -1.0, 0, ParameterInfo::kCanAutomate));

    parameters.addParameter(new RangeParameter(
        STR16("Final Output Trim"), kParamOutputGainDb, STR16("dB"),
        -12.0, 12.0, 0.0, 0, ParameterInfo::kCanAutomate));

    auto* inputParam = new StringListParameter(
        STR16("Input Routing"), kParamInputMode, nullptr,
        ParameterInfo::kCanAutomate | ParameterInfo::kIsList);
    inputParam->appendString(STR16("Auto (Match Host)"));
    inputParam->appendString(STR16("Mono Sum (L+R)"));
    inputParam->appendString(STR16("Left -> Both"));
    inputParam->appendString(STR16("Right -> Both"));
    parameters.addParameter(inputParam);

    return kResultOk;
}

tresult PLUGIN_API AutoGainNSController::setComponentState(IBStream* state) {
    VoiceParams p{};
    if (readState(state, p) != kResultOk) {
        return kResultFalse;
    }

    setParamNormalized(kParamBypass, p.bypass ? 1.0 : 0.0);
    setParamNormalized(kParamHpfHz, rangeToNorm(p.hpfHz, 20.0, 160.0));
    setParamNormalized(kParamNsStrength, std::clamp(static_cast<double>(p.nsStrength), 0.0, 1.0));
    setParamNormalized(kParamVadThreshold, rangeToNorm(p.vadThreshold, 0.0, 0.95));
    setParamNormalized(kParamGateFloorDb, rangeToNorm(p.gateFloorDb, -60.0, 0.0));
    setParamNormalized(kParamAgcEnable, p.agcEnable ? 1.0 : 0.0);
    setParamNormalized(kParamTargetLufs, rangeToNorm(p.targetLufs, -28.0, -10.0));
    setParamNormalized(kParamAgcMaxBoostDb, rangeToNorm(p.agcMaxBoostDb, 0.0, 30.0));
    setParamNormalized(kParamAgcSpeed, std::clamp(static_cast<double>(p.agcSpeed), 0.0, 1.0));
    setParamNormalized(kParamLimiterCeilingDb, rangeToNorm(p.limiterCeilingDb, -6.0, -0.1));
    setParamNormalized(kParamOutputGainDb, rangeToNorm(p.outputGainDb, -12.0, 12.0));
    setParamNormalized(kParamInputMode, static_cast<double>(std::clamp(p.inputMode, 0, 3)) / 3.0);

    return kResultOk;
}

IPlugView* PLUGIN_API AutoGainNSController::createView(FIDString name) {
    if (name && std::strcmp(name, ViewType::kEditor) == 0) {
        return new AutoGainNSView(this);
    }
    return nullptr;
}

tresult PLUGIN_API AutoGainNSController::setParamNormalized(Steinberg::Vst::ParamID tag, ParamValue value) {
    tresult res = EditControllerEx1::setParamNormalized(tag, value);
    if (auto* v = activeView_.load(std::memory_order_acquire)) {
        v->refresh();
    }
    return res;
}

void AutoGainNSController::resetDefaults() {
    const VoiceParams def{};
    const struct { ParamID id; double norm; } items[] = {
        { kParamBypass,           def.bypass ? 1.0 : 0.0 },
        { kParamHpfHz,            rangeToNorm(def.hpfHz, 20.0, 160.0) },
        { kParamNsStrength,       def.nsStrength },
        { kParamVadThreshold,     rangeToNorm(def.vadThreshold, 0.0, 0.95) },
        { kParamGateFloorDb,      rangeToNorm(def.gateFloorDb, -60.0, 0.0) },
        { kParamAgcEnable,        def.agcEnable ? 1.0 : 0.0 },
        { kParamTargetLufs,       rangeToNorm(def.targetLufs, -28.0, -10.0) },
        { kParamAgcMaxBoostDb,    rangeToNorm(def.agcMaxBoostDb, 0.0, 30.0) },
        { kParamAgcSpeed,         def.agcSpeed },
        { kParamLimiterCeilingDb, rangeToNorm(def.limiterCeilingDb, -6.0, -0.1) },
        { kParamOutputGainDb,     rangeToNorm(def.outputGainDb, -12.0, 12.0) },
        { kParamInputMode,        0.0 },
    };
    for (const auto& item : items) {
        beginEdit(item.id);
        setParamNormalized(item.id, item.norm);
        performEdit(item.id, item.norm);
        endEdit(item.id);
    }
}

void AutoGainNSController::applyPreset(int presetIndex) {
    if (presetIndex < 0 || presetIndex >= kNumPresets) return;
    const auto& p = kPresets[presetIndex];

    // Intentionally preserve kParamInputMode and kParamBypass
    const struct { ParamID id; double norm; } items[] = {
        { kParamHpfHz,            rangeToNorm(p.hpfHz, 20.0, 160.0) },
        { kParamNsStrength,       std::clamp(p.nsStrength, 0.0, 1.0) },
        { kParamVadThreshold,     rangeToNorm(p.vadThreshold, 0.0, 0.95) },
        { kParamGateFloorDb,      rangeToNorm(p.gateFloorDb, -60.0, 0.0) },
        { kParamAgcEnable,        p.agcEnable },
        { kParamTargetLufs,       rangeToNorm(p.targetLufs, -28.0, -10.0) },
        { kParamAgcMaxBoostDb,    rangeToNorm(p.agcMaxBoostDb, 0.0, 30.0) },
        { kParamAgcSpeed,         std::clamp(p.agcSpeed, 0.0, 1.0) },
        { kParamLimiterCeilingDb, rangeToNorm(p.limiterCeilingDb, -6.0, -0.1) },
        { kParamOutputGainDb,     rangeToNorm(p.outputGainDb, -12.0, 12.0) },
    };
    for (const auto& item : items) {
        beginEdit(item.id);
        setParamNormalized(item.id, item.norm);
        performEdit(item.id, item.norm);
        endEdit(item.id);
    }
}

} // namespace autogain
