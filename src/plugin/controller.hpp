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

    void setActiveView(AutoGainNSView* view) noexcept {
        activeView_.store(view, std::memory_order_release);
    }
    void clearActiveView(AutoGainNSView* view) noexcept {
        AutoGainNSView* expected = view;
        activeView_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }

    void resetDefaults();
    void applyPreset(int presetIndex);

private:
    std::atomic<AutoGainNSView*> activeView_{ nullptr };
};

} // namespace autogain
