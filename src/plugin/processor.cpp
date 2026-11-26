#include "plugin/processor.hpp"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/vst/vsttypes.h"
#include <algorithm>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace autogain {

AutoGainNSProcessor::AutoGainNSProcessor() {
    setControllerClass(kControllerUID);
}

tresult PLUGIN_API AutoGainNSProcessor::initialize(FUnknown* context) {
    tresult res = AudioEffect::initialize(context);
    if (res != kResultOk) return res;

    // Default to Mono for capture mics (also negotiates Mono->Stereo and Stereo->Stereo)
    addAudioInput(STR16("Audio In"), SpeakerArr::kMono);
    addAudioOutput(STR16("Audio Out"), SpeakerArr::kMono);
    return kResultOk;
}

tresult PLUGIN_API AutoGainNSProcessor::terminate() {
    return AudioEffect::terminate();
}

tresult PLUGIN_API AutoGainNSProcessor::setBusArrangements(
    SpeakerArrangement* inputs, int32 numIns,
    SpeakerArrangement* outputs, int32 numOuts) {
    if (numIns != 1 || numOuts != 1 || !inputs || !outputs) return kResultFalse;

    const bool validMonoMono     = (inputs[0] == SpeakerArr::kMono && outputs[0] == SpeakerArr::kMono);
    const bool validStereoStereo = (inputs[0] == SpeakerArr::kStereo && outputs[0] == SpeakerArr::kStereo);
    const bool validMonoStereo   = (inputs[0] == SpeakerArr::kMono && outputs[0] == SpeakerArr::kStereo);

    if (validMonoMono || validStereoStereo || validMonoStereo) {
        inChannels_ = SpeakerArr::getChannelCount(inputs[0]);
        outChannels_ = SpeakerArr::getChannelCount(outputs[0]);
        AudioBus* inBus = FCast<AudioBus>(audioInputs.at(0));
        AudioBus* outBus = FCast<AudioBus>(audioOutputs.at(0));
        if (inBus) inBus->setArrangement(inputs[0]);
        if (outBus) outBus->setArrangement(outputs[0]);
        return kResultTrue;
    }
    return kResultFalse;
}

tresult PLUGIN_API AutoGainNSProcessor::canProcessSampleSize(int32 symbolicSampleSize) {
    return (symbolicSampleSize == kSample32) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API AutoGainNSProcessor::setupProcessing(ProcessSetup& newSetup) {
    sampleRate_ = newSetup.sampleRate;
    return AudioEffect::setupProcessing(newSetup);
}

tresult PLUGIN_API AutoGainNSProcessor::setActive(TBool state) {
    if (state) {
        SpeakerArrangement inArr = SpeakerArr::kMono;
        SpeakerArrangement outArr = SpeakerArr::kMono;
        getBusArrangement(kInput, 0, inArr);
        getBusArrangement(kOutput, 0, outArr);
        inChannels_ = std::clamp(SpeakerArr::getChannelCount(inArr), 1, 2);
        outChannels_ = std::clamp(SpeakerArr::getChannelCount(outArr), 1, 2);
        dsp_.prepare(sampleRate_, std::max(inChannels_, outChannels_));
        dsp_.setParams(params_);
    } else {
        dsp_.reset();
    }
    return AudioEffect::setActive(state);
}

uint32 PLUGIN_API AutoGainNSProcessor::getLatencySamples() {
    return static_cast<uint32>(dsp_.getLatencySamples());
}

tresult PLUGIN_API AutoGainNSProcessor::process(ProcessData& data) {
    if (data.inputParameterChanges) {
        const int32 numParamsChanged = data.inputParameterChanges->getParameterCount();
        for (int32 i = 0; i < numParamsChanged; ++i) {
            IParamValueQueue* queue = data.inputParameterChanges->getParameterData(i);
            if (!queue) continue;
            ParamValue value = 0.0;
            int32 sampleOffset = 0;
            const int32 numPoints = queue->getPointCount();
            if (numPoints > 0 && queue->getPoint(numPoints - 1, sampleOffset, value) == kResultTrue) {
                applyParamNormalized(params_, queue->getParameterId(), value);
            }
        }
        dsp_.setParams(params_);
    }

    if (data.numInputs == 0 || data.numOutputs == 0 || data.numSamples <= 0 ||
        !data.inputs || !data.outputs) {
        return kResultOk;
    }

    const int32 inCh = std::clamp(data.inputs[0].numChannels, 0, 2);
    const int32 outCh = std::clamp(data.outputs[0].numChannels, 0, 2);
    if (inCh <= 0 || outCh <= 0) return kResultOk;

    float* const* inBufs = data.inputs[0].channelBuffers32;
    float* const* outBufs = data.outputs[0].channelBuffers32;
    if (!inBufs || !outBufs) return kResultOk;

    for (int32 ch = 0; ch < inCh; ++ch) {
        if (!inBufs[ch]) return kResultOk;
    }
    for (int32 ch = 0; ch < outCh; ++ch) {
        if (!outBufs[ch]) return kResultOk;
    }

    if (data.symbolicSampleSize == kSample32) {
        dsp_.process(inBufs, outBufs, inCh, outCh, data.numSamples);
    }

    return kResultOk;
}

tresult PLUGIN_API AutoGainNSProcessor::setState(IBStream* state) {
    VoiceParams loaded{};
    if (readState(state, loaded) != kResultOk) {
        return kResultFalse;
    }
    params_ = loaded;
    dsp_.setParams(params_);
    return kResultOk;
}

tresult PLUGIN_API AutoGainNSProcessor::getState(IBStream* state) {
    return writeState(state, params_);
}

} // namespace autogain
