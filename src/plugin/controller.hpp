#pragma once

#include "public.sdk/source/vst/vsteditcontroller.h"
#include "plugin/ids.hpp"
#include <atomic>

namespace autogain {

class AutoGainNSView;

class AutoGainNSController : public Steinberg::Vst::EditControllerEx1 {
public:
    AutoGainNSController() = default;
    ~AutoGainNSController() override = default;

    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IEditController*>(new AutoGainNSController());
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API setComponentState(Steinberg::IBStream* state) override;
    Steinberg::IPlugView* PLUGIN_API createView(Steinberg::FIDString name) override;
    Steinberg::tresult PLUGIN_API setParamNormalized(
        Steinberg::Vst::ParamID tag, Steinberg::Vst::ParamValue value) override;
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override;

    void setActiveView(AutoGainNSView* view) noexcept {
        activeView_.store(view, std::memory_order_release);
    }
    void clearActiveView(AutoGainNSView* view) noexcept {
        AutoGainNSView* expected = view;
        activeView_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }

    void resetDefaults();
    void applyPreset(int presetIndex);
    void requestTelemetry();

    float telemetryInDb() const noexcept { return telInDb_.load(std::memory_order_relaxed); }
    float telemetryOutDb() const noexcept { return telOutDb_.load(std::memory_order_relaxed); }
    float telemetryAgcDb() const noexcept { return telAgcDb_.load(std::memory_order_relaxed); }
    float telemetryGateGain() const noexcept { return telGateGain_.load(std::memory_order_relaxed); }

private:
    std::atomic<AutoGainNSView*> activeView_{ nullptr };
    std::atomic<float> telInDb_{ -120.0f };
    std::atomic<float> telOutDb_{ -120.0f };
    std::atomic<float> telAgcDb_{ 0.0f };
    std::atomic<float> telGateGain_{ 0.0f };
};

} // namespace autogain
