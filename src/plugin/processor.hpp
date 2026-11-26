#pragma once

#include "public.sdk/source/vst/vstaudioeffect.h"
#include "dsp/voice_processor.hpp"
#include "plugin/ids.hpp"

namespace autogain {

class AutoGainNSProcessor : public Steinberg::Vst::AudioEffect {
public:
    AutoGainNSProcessor();
    ~AutoGainNSProcessor() override = default;

    static Steinberg::FUnknown* createInstance(void*) {
        return static_cast<Steinberg::Vst::IAudioProcessor*>(new AutoGainNSProcessor());
    }

    Steinberg::tresult PLUGIN_API initialize(Steinberg::FUnknown* context) override;
    Steinberg::tresult PLUGIN_API terminate() override;
    Steinberg::tresult PLUGIN_API setBusArrangements(
        Steinberg::Vst::SpeakerArrangement* inputs, Steinberg::int32 numIns,
        Steinberg::Vst::SpeakerArrangement* outputs, Steinberg::int32 numOuts) override;
    Steinberg::tresult PLUGIN_API canProcessSampleSize(Steinberg::int32 symbolicSampleSize) override;
    Steinberg::tresult PLUGIN_API setupProcessing(Steinberg::Vst::ProcessSetup& newSetup) override;
    Steinberg::tresult PLUGIN_API setActive(Steinberg::TBool state) override;
    Steinberg::tresult PLUGIN_API setProcessing(Steinberg::TBool state) override { (void)state; return Steinberg::kResultOk; }
    Steinberg::uint32 PLUGIN_API getLatencySamples() override;
    Steinberg::tresult PLUGIN_API process(Steinberg::Vst::ProcessData& data) override;
    Steinberg::tresult PLUGIN_API setState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API getState(Steinberg::IBStream* state) override;
    Steinberg::tresult PLUGIN_API notify(Steinberg::Vst::IMessage* message) override;

private:
    VoiceParams params_{};
    VoiceProcessor dsp_{};
    double sampleRate_ = 48000.0;
    Steinberg::int32 inChannels_ = 1;
    Steinberg::int32 outChannels_ = 1;
};

} // namespace autogain
