#define NOMINMAX
#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include <algorithm>

#include "dsp/voice_processor.hpp"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/vsttypes.h"
#include "plugin/ids.hpp"

#define TEST_CHECK(cond) do { \
    if (!(cond)) { \
        std::fprintf(stderr, "FAILED at %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        std::exit(1); \
    } \
} while (0)

using namespace autogain;

namespace {

constexpr double kPi = 3.14159265358979323846;

class MemStream : public Steinberg::IBStream {
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID _iid, void** obj) override {
        if (Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::IBStream::iid) ||
            Steinberg::FUnknownPrivate::iidEqual(_iid, Steinberg::FUnknown::iid)) {
            *obj = this;
            addRef();
            return Steinberg::kResultOk;
        }
        *obj = nullptr;
        return Steinberg::kNoInterface;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++ref_; }
    Steinberg::uint32 PLUGIN_API release() override { return --ref_; }

    Steinberg::tresult PLUGIN_API read(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesRead) override {
        const size_t avail = (pos_ < data_.size()) ? (data_.size() - pos_) : 0;
        const size_t n = std::min(static_cast<size_t>(numBytes), avail);
        if (n > 0) {
            std::memcpy(buffer, data_.data() + pos_, n);
            pos_ += n;
        }
        if (numBytesRead) *numBytesRead = static_cast<Steinberg::int32>(n);
        return (n == static_cast<size_t>(numBytes)) ? Steinberg::kResultOk : Steinberg::kResultFalse;
    }
    Steinberg::tresult PLUGIN_API write(void* buffer, Steinberg::int32 numBytes, Steinberg::int32* numBytesWritten) override {
        const auto* b = static_cast<const unsigned char*>(buffer);
        if (pos_ + static_cast<size_t>(numBytes) > data_.size()) {
            data_.resize(pos_ + static_cast<size_t>(numBytes));
        }
        std::memcpy(data_.data() + pos_, b, static_cast<size_t>(numBytes));
        pos_ += static_cast<size_t>(numBytes);
        if (numBytesWritten) *numBytesWritten = numBytes;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 offset, Steinberg::int32 mode, Steinberg::int64* result) override {
        Steinberg::int64 next = static_cast<Steinberg::int64>(pos_);
        if (mode == kIBSeekSet) next = offset;
        else if (mode == kIBSeekCur) next += offset;
        else if (mode == kIBSeekEnd) next = static_cast<Steinberg::int64>(data_.size()) + offset;
        if (next < 0) return Steinberg::kResultFalse;
        pos_ = static_cast<size_t>(next);
        if (result) *result = next;
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* pos) override {
        if (pos) *pos = static_cast<Steinberg::int64>(pos_);
        return Steinberg::kResultOk;
    }

private:
    Steinberg::uint32 ref_ = 1;
    std::vector<unsigned char> data_;
    size_t pos_ = 0;
};

double computeRmsDb(const std::vector<float>& buf, int start, int end) {
    double sum = 0.0;
    const int n = std::max(1, end - start);
    for (int i = start; i < end; ++i) {
        sum += static_cast<double>(buf[i]) * static_cast<double>(buf[i]);
    }
    return 10.0 * std::log10(std::max(sum / n, 1e-12));
}

void testNoiseSuppression() {
    std::printf("[Test 1] Noise Suppression (RNNoise + HPF + VAD Gate)...\n");
    VoiceProcessor proc;
    proc.prepare(48000.0, 1);
    VoiceParams p;
    p.agcEnable = false;
    p.nsStrength = 1.0f;
    p.vadThreshold = 0.50f;
    p.gateFloorDb = -24.0f;
    proc.setParams(p);

    const int fs = 48000;
    const int n = fs * 2;
    std::vector<float> in(n, 0.0f), out(n, 0.0f);

    uint32_t rng = 0x12345678u;
    for (int i = 0; i < n; ++i) {
        rng = rng * 1664525u + 1013904223u;
        float white = (static_cast<float>((rng >> 16) & 0xFFFF) / 32768.0f - 1.0f) * 0.03f;
        float hum = 0.02f * static_cast<float>(std::sin(2.0 * kPi * 60.0 * i / fs));
        in[i] = white + hum;
    }

    for (int pos = 0; pos < n; pos += 480) {
        float* iArr[1] = { in.data() + pos };
        float* oArr[1] = { out.data() + pos };
        proc.process(iArr, oArr, 1, std::min(480, n - pos));
    }

    const double inDb = computeRmsDb(in, fs, n);
    const double outDb = computeRmsDb(out, fs, n);
    std::printf("  Noise-only In: %.2f dBFS -> Out: %.2f dBFS (Reduction: %.2f dB)\n",
                inDb, outDb, inDb - outDb);
    TEST_CHECK(outDb < inDb - 15.0);
}

void testSpeechGatedAutoGain() {
    std::printf("[Test 2] Speech-Gated AutoGain (LUFS convergence)...\n");
    VoiceProcessor proc;
    proc.prepare(48000.0, 1);
    VoiceParams p;
    p.nsStrength = 0.0f;
    p.vadThreshold = 0.0f;
    p.agcEnable = true;
    p.targetLufs = -16.0f;
    p.agcMaxBoostDb = 18.0f;
    p.agcSpeed = 0.8f;
    proc.setParams(p);

    const int fs = 48000;
    const int n = fs * 4;
    std::vector<float> in(n, 0.0f), out(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        double t = static_cast<double>(i) / fs;
        double s = 0.025 * std::sin(2.0 * kPi * 180.0 * t)
                 + 0.015 * std::sin(2.0 * kPi * 360.0 * t)
                 + 0.010 * std::sin(2.0 * kPi * 900.0 * t);
        in[i] = static_cast<float>(s);
    }

    for (int pos = 0; pos < n; pos += 480) {
        float* iArr[1] = { in.data() + pos };
        float* oArr[1] = { out.data() + pos };
        proc.process(iArr, oArr, 1, std::min(480, n - pos));
    }

    const double inDb = computeRmsDb(in, fs * 3, n);
    const double outDb = computeRmsDb(out, fs * 3, n);
    std::printf("  Quiet Speech In: %.2f dBFS -> Out: %.2f dBFS (AGC Gain: %+.2f dB)\n",
                inDb, outDb, proc.currentAgcGainDb());
    TEST_CHECK(outDb > inDb + 8.0);
    TEST_CHECK(proc.currentAgcGainDb() > 8.0);
}

void testBrickwallLimiter() {
    std::printf("[Test 3] 2ms Sliding-Window Lookahead Limiter (+12 dBFS transient)...\n");
    VoiceProcessor proc;
    proc.prepare(48000.0, 1);
    VoiceParams p;
    p.nsStrength = 0.0f;
    p.vadThreshold = 0.0f;
    p.agcEnable = false;
    p.limiterCeilingDb = -1.0f;
    proc.setParams(p);

    const int n = 4800;
    std::vector<float> in(n, 0.0f), out(n, 0.0f);
    for (int i = 1000; i < 1500; ++i) {
        in[i] = 4.0f * static_cast<float>(std::sin(2.0 * kPi * 1000.0 * i / 48000.0));
    }

    for (int pos = 0; pos < n; pos += 256) {
        float* iArr[1] = { in.data() + pos };
        float* oArr[1] = { out.data() + pos };
        proc.process(iArr, oArr, 1, std::min(256, n - pos));
    }

    float maxAbs = 0.0f;
    for (float s : out) maxAbs = std::max(maxAbs, std::abs(s));
    double maxDb = 20.0 * std::log10(std::max(maxAbs, 1e-12f));
    std::printf("  Peak Out: %.3f (%.2f dBFS, Ceiling: -1.00 dBFS)\n", maxAbs, maxDb);
    TEST_CHECK(maxDb <= -0.95);

    // 4x True-Peak check: Fs/4 inter-sample peak (+0.88, +0.88, -0.88, -0.88) has sample peak -1.11 dBFS
    // but true inter-sample peak ~+1.9 dBFS; True-Peak limiter must attenuate it below -1.0 dBFS.
    proc.reset();
    std::vector<float> ispIn(2048, 0.0f), ispOut(2048, 0.0f);
    for (int i = 600; i < 1200; ++i) {
        ispIn[i] = ((i & 2) == 0) ? 0.88f : -0.88f;
    }
    for (int pos = 0; pos < 2048; pos += 256) {
        float* iArr[1] = { ispIn.data() + pos };
        float* oArr[1] = { ispOut.data() + pos };
        proc.process(iArr, oArr, 1, 256);
    }
    float ispSampleMax = 0.0f;
    for (int i = 1250; i < 1650; ++i) ispSampleMax = std::max(ispSampleMax, std::abs(ispOut[i]));
    std::printf("  4x True-Peak Out (from 0.88 Fs/4): %.3f\n", ispSampleMax);
    TEST_CHECK(ispSampleMax < 0.78f); // attenuated below 0.88 despite sample peak (-1.11 dBFS) < ceiling (-1.00 dBFS)
}

void testBlockSizesAndSafety() {
    std::printf("[Test 4] Arbitrary block sizes (64..1024) & Mono/Stereo NaN/Inf safety...\n");
    const int sizes[] = {64, 128, 256, 480, 512, 1024};
    for (int ch = 1; ch <= 2; ++ch) {
        for (int bs : sizes) {
            VoiceProcessor proc;
            proc.prepare(48000.0, ch);
            VoiceParams p;
            proc.setParams(p);

            std::vector<float> in0(bs, 0.05f), in1(bs, -0.05f);
            std::vector<float> out0(bs, 0.0f), out1(bs, 0.0f);
            float* ins[2] = { in0.data(), in1.data() };
            float* outs[2] = { out0.data(), out1.data() };

            for (int iter = 0; iter < 10; ++iter) {
                proc.process(ins, outs, ch, bs);
                for (int c = 0; c < ch; ++c) {
                    for (int i = 0; i < bs; ++i) {
                        TEST_CHECK(std::isfinite(outs[c][i]));
                        TEST_CHECK(std::abs(outs[c][i]) <= 1.0f);
                    }
                }
            }
        }
    }
    std::printf("  All block sizes & channels clean.\n");
}

void testSilenceToSpeechNoSurge() {
    std::printf("[Test 5] Silence-to-Speech Freeze (no gain surge after silence)...\n");
    VoiceProcessor proc;
    proc.prepare(48000.0, 1);
    VoiceParams p;
    p.nsStrength = 0.0f;
    p.vadThreshold = 0.0f;
    p.agcEnable = true;
    p.targetLufs = -16.0f;
    p.agcMaxBoostDb = 18.0f;
    p.agcSpeed = 0.8f;
    proc.setParams(p);

    const int fs = 48000;
    std::vector<float> speech(fs * 2, 0.0f), silence(fs * 3, 0.0f), out(fs * 3, 0.0f);
    for (int i = 0; i < fs * 2; ++i) {
        speech[i] = 0.05f * static_cast<float>(std::sin(2.0 * kPi * 300.0 * i / fs));
    }

    // 2s speech
    for (int pos = 0; pos < fs * 2; pos += 480) {
        float* iArr[1] = { speech.data() + pos };
        float* oArr[1] = { out.data() + pos };
        proc.process(iArr, oArr, 1, 480);
    }
    const double gainAfterSpeech = proc.currentAgcGainDb();

    // 3s silence
    for (int pos = 0; pos < fs * 3; pos += 480) {
        float* iArr[1] = { silence.data() + pos };
        float* oArr[1] = { out.data() + pos };
        proc.process(iArr, oArr, 1, 480);
    }
    const double gainAfterSilence = proc.currentAgcGainDb();
    std::printf("  AGC Gain after speech: %+.2f dB -> after 3s silence: %+.2f dB\n",
                gainAfterSpeech, gainAfterSilence);
    TEST_CHECK(std::abs(gainAfterSilence - gainAfterSpeech) < 0.15);
}

void testOutputGainLimiterGuard() {
    std::printf("[Test 6] Output Gain (+12 dB) + Limiter Ceiling Guard (-1.0 dBFS)...\n");
    VoiceProcessor proc;
    proc.prepare(48000.0, 1);
    VoiceParams p;
    p.nsStrength = 0.0f;
    p.vadThreshold = 0.0f;
    p.agcEnable = false;
    p.outputGainDb = 12.0f;
    p.limiterCeilingDb = -1.0f;
    proc.setParams(p);

    const int n = 4800;
    std::vector<float> in(n, 0.0f), out(n, 0.0f);
    for (int i = 0; i < n; ++i) {
        in[i] = 0.5f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * i / 48000.0));
    }
    for (int pos = 0; pos < n; pos += 480) {
        float* iArr[1] = { in.data() + pos };
        float* oArr[1] = { out.data() + pos };
        proc.process(iArr, oArr, 1, 480);
    }

    float maxAbs = 0.0f;
    for (float s : out) maxAbs = std::max(maxAbs, std::abs(s));
    double maxDb = 20.0 * std::log10(std::max(maxAbs, 1e-12f));
    std::printf("  Peak Out with +12dB Output Gain: %.3f (%.2f dBFS)\n", maxAbs, maxDb);
    TEST_CHECK(maxDb <= -0.95);
}

void testNanAndMultiSampleRates() {
    std::printf("[Test 7] NaN/Inf Resilience + 44.1 kHz & 96 kHz Resampling...\n");
    const double rates[] = {44100.0, 48000.0, 96000.0};
    for (double sr : rates) {
        VoiceProcessor proc;
        proc.prepare(sr, 1);
        VoiceParams p;
        proc.setParams(p);

        const int n = 2048;
        std::vector<float> in(n, 0.0f), out(n, 0.0f);
        for (int i = 0; i < n; ++i) {
            in[i] = 0.1f * static_cast<float>(std::sin(2.0 * kPi * 440.0 * i / sr));
        }
        // Inject NaN & +Inf mid-stream
        in[500] = std::numeric_limits<float>::quiet_NaN();
        in[501] = std::numeric_limits<float>::infinity();
        in[502] = -std::numeric_limits<float>::infinity();

        for (int pos = 0; pos < n; pos += 256) {
            float* iArr[1] = { in.data() + pos };
            float* oArr[1] = { out.data() + pos };
            proc.process(iArr, oArr, 1, 256);
        }

        for (float s : out) {
            TEST_CHECK(std::isfinite(s));
            TEST_CHECK(std::abs(s) <= 1.0f);
        }
    }
    std::printf("  44.1 kHz, 48 kHz, 96 kHz + NaN/Inf recovery clean.\n");
}

void testVst3BinaryLoad() {
    std::printf("[Test 8] VST3 DLL Load, State Round-Trip, Mono/Stereo Bus, Process & IPlugView GUI...\n");
    const char* dllPath = "out\\AutoGainNS.vst3\\Contents\\x86_64-win\\AutoGainNS.vst3";
    HMODULE mod = LoadLibraryA(dllPath);
    TEST_CHECK(mod != nullptr);

    using InitFn = bool(__stdcall*)();
    using ExitFn = bool(__stdcall*)();
    using FactoryFn = Steinberg::IPluginFactory*(__stdcall*)();

    auto initFn = reinterpret_cast<InitFn>(GetProcAddress(mod, "InitDll"));
    auto exitFn = reinterpret_cast<ExitFn>(GetProcAddress(mod, "ExitDll"));
    auto factoryFn = reinterpret_cast<FactoryFn>(GetProcAddress(mod, "GetPluginFactory"));
    TEST_CHECK(initFn && exitFn && factoryFn);
    TEST_CHECK(initFn());

    Steinberg::IPluginFactory* factory = factoryFn();
    TEST_CHECK(factory != nullptr);
    TEST_CHECK(factory->countClasses() == 2);

    Steinberg::Vst::IComponent* comp = nullptr;
    Steinberg::tresult res = factory->createInstance(
        kProcessorUID.toTUID(), Steinberg::Vst::IComponent::iid, reinterpret_cast<void**>(&comp));
    TEST_CHECK(res == Steinberg::kResultOk && comp != nullptr);
    TEST_CHECK(comp->initialize(nullptr) == Steinberg::kResultOk);

    Steinberg::Vst::IAudioProcessor* audio = nullptr;
    TEST_CHECK(comp->queryInterface(Steinberg::Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&audio)) == Steinberg::kResultOk);

    Steinberg::Vst::SpeakerArrangement mono = Steinberg::Vst::SpeakerArr::kMono;
    TEST_CHECK(audio->setBusArrangements(&mono, 1, &mono, 1) == Steinberg::kResultTrue);

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;
    setup.maxSamplesPerBlock = 512;
    setup.sampleRate = 48000.0;
    TEST_CHECK(audio->setupProcessing(setup) == Steinberg::kResultOk);
    TEST_CHECK(comp->setActive(true) == Steinberg::kResultOk);
    TEST_CHECK(audio->setProcessing(true) == Steinberg::kResultOk);
    TEST_CHECK(audio->getLatencySamples() == 576);

    // State save & restore round-trip
    MemStream stream;
    TEST_CHECK(comp->getState(&stream) == Steinberg::kResultOk);
    Steinberg::int64 zero = 0;
    TEST_CHECK(stream.seek(0, Steinberg::IBStream::kIBSeekSet, &zero) == Steinberg::kResultOk);
    TEST_CHECK(comp->setState(&stream) == Steinberg::kResultOk);

    std::vector<float> in(512, 0.01f), out(512, 0.0f);
    float* inPtr = in.data();
    float* outPtr = out.data();
    Steinberg::Vst::AudioBusBuffers inBus{}, outBus{};
    inBus.numChannels = 1; inBus.channelBuffers32 = &inPtr;
    outBus.numChannels = 1; outBus.channelBuffers32 = &outPtr;

    Steinberg::Vst::ProcessData data{};
    data.processMode = Steinberg::Vst::kRealtime;
    data.symbolicSampleSize = Steinberg::Vst::kSample32;
    data.numSamples = 512;
    data.numInputs = 1; data.inputs = &inBus;
    data.numOutputs = 1; data.outputs = &outBus;
    TEST_CHECK(audio->process(data) == Steinberg::kResultOk);

    TEST_CHECK(audio->setProcessing(false) == Steinberg::kResultOk);
    TEST_CHECK(comp->setActive(false) == Steinberg::kResultOk);

    Steinberg::Vst::SpeakerArrangement stereo = Steinberg::Vst::SpeakerArr::kStereo;
    TEST_CHECK(audio->setBusArrangements(&stereo, 1, &stereo, 1) == Steinberg::kResultTrue);

    audio->release();
    comp->terminate();
    comp->release();

    // Controller + setComponentState + IPlugView GUI
    Steinberg::Vst::IEditController* ctrl = nullptr;
    res = factory->createInstance(
        kControllerUID.toTUID(), Steinberg::Vst::IEditController::iid, reinterpret_cast<void**>(&ctrl));
    TEST_CHECK(res == Steinberg::kResultOk && ctrl != nullptr);
    TEST_CHECK(ctrl->initialize(nullptr) == Steinberg::kResultOk);
    TEST_CHECK(ctrl->getParameterCount() == kNumParams);

    TEST_CHECK(stream.seek(0, Steinberg::IBStream::kIBSeekSet, &zero) == Steinberg::kResultOk);
    TEST_CHECK(ctrl->setComponentState(&stream) == Steinberg::kResultOk);

    Steinberg::IPlugView* view = ctrl->createView(Steinberg::Vst::ViewType::kEditor);
    TEST_CHECK(view != nullptr);
    TEST_CHECK(view->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) == Steinberg::kResultTrue);
    Steinberg::ViewRect vr{};
    TEST_CHECK(view->getSize(&vr) == Steinberg::kResultOk);
    TEST_CHECK(vr.getWidth() == 540 && vr.getHeight() == 610);

    Steinberg::IPlugViewContentScaleSupport* scaleSup = nullptr;
    TEST_CHECK(view->queryInterface(Steinberg::IPlugViewContentScaleSupport::iid, reinterpret_cast<void**>(&scaleSup)) == Steinberg::kResultOk && scaleSup != nullptr);
    TEST_CHECK(scaleSup->setContentScaleFactor(1.5f) == Steinberg::kResultOk);
    TEST_CHECK(view->getSize(&vr) == Steinberg::kResultOk);
    TEST_CHECK(vr.getWidth() == 810 && vr.getHeight() == 915);
    scaleSup->release();

    view->release();

    ctrl->terminate();
    ctrl->release();

    factory->release();
    TEST_CHECK(exitFn());
    FreeLibrary(mod);
    std::printf("  VST3 binary, state round-trip, Mono/Stereo, and 540x610 HiDPI IPlugView verified.\n");
}

void testUniversalEdgeCases() {
    std::printf("[Test 9] Multi-rate Wet/Dry Sync, Mono->Stereo / LeftOnly, State Clamping, Quiet Dynamic Mic...\n");

    // 1. 96 kHz & 44.1 kHz Wet/Dry alignment (no 10ms desync)
    for (double sr : { 44100.0, 96000.0 }) {
        VoiceProcessor vp;
        vp.prepare(sr, 1);
        VoiceParams p;
        p.hpfHz = 20.0f;
        p.nsStrength = 0.5f;
        p.vadThreshold = 0.0f;
        p.agcEnable = false;
        vp.setParams(p);

        const int total = 4096;
        std::vector<float> in(total, 0.0f), out(total, 0.0f);
        in[200] = 0.5f;
        float* inP = in.data();
        float* outP = out.data();
        vp.process(&inP, &outP, 1, total);

        const int expectedPos = 200 + vp.getLatencySamples();
        int peakIdx = 0;
        float peakVal = 0.0f;
        for (int i = 0; i < total; ++i) {
            if (std::abs(out[i]) > peakVal) {
                peakVal = std::abs(out[i]);
                peakIdx = i;
            }
        }
        TEST_CHECK(std::abs(peakIdx - expectedPos) <= 6);
        // Check no secondary echo >= 30% of main peak 10ms later
        const int echoCenter = expectedPos + static_cast<int>(sr * 0.010);
        float echoMax = 0.0f;
        for (int i = std::max(0, echoCenter - 32); i < std::min(total, echoCenter + 32); ++i) {
            echoMax = std::max(echoMax, std::abs(out[i]));
        }
        TEST_CHECK(echoMax < peakVal * 0.25f);
    }

    // 2. LeftOnly routing fixes 2-ch interface with mic only on Left
    {
        VoiceProcessor vp;
        vp.prepare(48000.0, 2);
        VoiceParams p;
        p.inputMode = kInputLeftOnly;
        p.vadThreshold = 0.0f;
        p.nsStrength = 0.0f;
        p.agcEnable = false;
        vp.setParams(p);

        const int n = 2048;
        std::vector<float> inL(n, 0.0f), inR(n, 0.0f), outL(n, 0.0f), outR(n, 0.0f);
        for (int i = 0; i < n; ++i) {
            inL[i] = 0.2f * std::sin(2.0f * 3.14159265f * 300.0f * i / 48000.0f);
        }
        float* ins[2] = { inL.data(), inR.data() };
        float* outs[2] = { outL.data(), outR.data() };
        vp.process(ins, outs, 2, 2, n);

        double rmsR = 0.0;
        for (int i = 1024; i < n; ++i) {
            TEST_CHECK(std::abs(outL[i] - outR[i]) < 1e-6f);
            rmsR += static_cast<double>(outR[i]) * outR[i];
        }
        rmsR = std::sqrt(rmsR / 1024.0);
        TEST_CHECK(rmsR > 0.05);
    }

    // 3. StateBlob round-trip & NaN clamping
    {
        MemStream s;
        VoiceParams orig{};
        orig.hpfHz = 115.0f;
        orig.agcMaxBoostDb = 24.0f;
        orig.inputMode = kInputLeftOnly;
        TEST_CHECK(writeState(&s, orig) == Steinberg::kResultOk);
        Steinberg::int64 zero = 0;
        TEST_CHECK(s.seek(0, Steinberg::IBStream::kIBSeekSet, &zero) == Steinberg::kResultOk);
        VoiceParams loaded{};
        TEST_CHECK(readState(&s, loaded) == Steinberg::kResultOk);
        TEST_CHECK(std::abs(loaded.hpfHz - 115.0f) < 1e-4f);
        TEST_CHECK(std::abs(loaded.agcMaxBoostDb - 24.0f) < 1e-4f);
        TEST_CHECK(loaded.inputMode == kInputLeftOnly);

        MemStream sBad;
        StateBlob bad{};
        bad.values[0] = std::numeric_limits<float>::quiet_NaN();
        bad.values[5] = 999.0f;
        bad.inputMode = 42;
        Steinberg::int32 nw = 0;
        TEST_CHECK(sBad.write(&bad, sizeof(bad), &nw) == Steinberg::kResultOk);
        TEST_CHECK(sBad.seek(0, Steinberg::IBStream::kIBSeekSet, &zero) == Steinberg::kResultOk);
        VoiceParams clamped{};
        TEST_CHECK(readState(&sBad, clamped) == Steinberg::kResultOk);
        TEST_CHECK(std::isfinite(clamped.hpfHz));
        TEST_CHECK(std::abs(clamped.agcMaxBoostDb - 30.0f) < 1e-4f);
        TEST_CHECK(clamped.inputMode == kInputRightOnly);
    }

    // 4. Default preset matches VoiceParams{} defaults
    {
        const VoiceParams def{};
        const auto& pr = kPresets[0];
        TEST_CHECK(std::abs(pr.hpfHz - def.hpfHz) < 1e-4);
        TEST_CHECK(std::abs(pr.nsStrength - def.nsStrength) < 1e-4);
        TEST_CHECK(std::abs(pr.vadThreshold - def.vadThreshold) < 1e-4);
        TEST_CHECK(std::abs(pr.gateFloorDb - def.gateFloorDb) < 1e-4);
        TEST_CHECK(std::abs(pr.targetLufs - def.targetLufs) < 1e-4);
        TEST_CHECK(std::abs(pr.agcMaxBoostDb - def.agcMaxBoostDb) < 1e-4);
        TEST_CHECK(std::abs(pr.agcSpeed - def.agcSpeed) < 1e-4);
        TEST_CHECK(std::abs(pr.limiterCeilingDb - def.limiterCeilingDb) < 1e-4);
        TEST_CHECK(std::abs(pr.outputGainDb - def.outputGainDb) < 1e-4);
    }

    // 5. Quiet dynamic mic (-56 dBFS) AGC activation
    {
        VoiceProcessor vp;
        vp.prepare(48000.0, 1);
        VoiceParams p;
        p.vadThreshold = 0.0f;
        p.nsStrength = 0.0f;
        p.agcEnable = true;
        p.targetLufs = -16.0f;
        p.agcMaxBoostDb = 30.0f;
        p.agcSpeed = 1.0f;
        vp.setParams(p);

        const int n = 48000 * 3;
        std::vector<float> in(n, 0.0f), out(n, 0.0f);
        for (int i = 0; i < n; ++i) {
            in[i] = 0.001585f * std::sin(2.0f * 3.14159265f * 250.0f * i / 48000.0f);
        }
        float* inP = in.data();
        float* outP = out.data();
        for (int off = 0; off < n; off += 480) {
            float* iPtr = inP + off;
            float* oPtr = outP + off;
            vp.process(&iPtr, &oPtr, 1, 480);
        }
        TEST_CHECK(vp.currentAgcGainDb() > 20.0);
    }

    std::printf("  Multi-rate sync, 2-ch interface routing, state clamping, Balanced preset, and quiet mic AGC verified.\n");
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    testNoiseSuppression();
    testSpeechGatedAutoGain();
    testBrickwallLimiter();
    testBlockSizesAndSafety();
    testSilenceToSpeechNoSurge();
    testOutputGainLimiterGuard();
    testNanAndMultiSampleRates();
    testVst3BinaryLoad();
    testUniversalEdgeCases();
    std::printf("\nALL 9 TESTS PASSED.\n");
    return 0;
}
