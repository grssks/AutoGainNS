# AutoGainNS

A lightweight, low-CPU 64-bit Windows VST3 voice plugin for microphones. Designed to run always-on in real time without a GPU, heavy frameworks, or external runtime DLLs. It combines AI background noise removal, a speech-aware gate, automatic volume leveling, and clip protection in one window.

- **Low CPU & zero runtime dependencies:** Allocation-free real-time `process()`, static C++ runtime (`/MT`), and standard Windows system DLLs only (`KERNEL32`, `USER32`, `GDI32`, `ole32`). The `.vst3` binary is `~15 MB` because RNNoise's neural weights are embedded directly inside (no external model files needed).
- **Universal x64 CPU support:** Baseline x64 (SSE2) with runtime SIMD dispatch (`SSE4.1` / `AVX2` for RNNoise via CPUID): fast on modern CPUs, safe on older ones.
- **12 ms latency** (`576` samples @ 48 kHz: `10 ms` RNNoise frame + `2 ms` limiter lookahead).
- **Multi-sample-rate:** Native `48 kHz` fast path plus anti-aliased resampling (`16 / 44.1 / 88.2 / 96 / 192 kHz`).

---

## Processing Chain

```text
Mic Input ─▶ Input Routing ─▶ Low-Cut Filter ─▶ AI Noise Removal ─▶ Voice Gate ─▶ Auto Volume (LUFS) ─▶ Output Trim ─▶ 2ms Lookahead Limiter ─▶ Output
```

1. **Input Routing & Sanitizer:** Drops `NaN`/`Inf` spikes and handles 1-channel mics, 2-channel audio interfaces (`Left -> Both` / `Right -> Both`), or stereo streams.
2. **Low-Cut Rumble Filter:** 2nd-order Butterworth high-pass filter (`20–160 Hz`) removes desk thumps, AC hum, and wind rumble.
3. **AI Noise Removal:** Embedded Xiph.Org RNNoise reduces fans, keyboard clicks, and room hiss with phase-aligned Wet/Dry blending.
4. **Voice Gate:** Per-sample neural Voice Activity Detection (`2 ms` attack, `150 ms` hold, `180 ms` release) keeps word starts intact while lowering background noise during pauses.
5. **Auto Volume (ITU-R BS.1770-4 LUFS):** Measures loudness only while you speak and freezes during silence so room noise never pumps up between sentences. Supports up to `+30 dB` boost for quiet dynamic mics.
6. **Clip Protection:** `2 ms` 4x True-Peak lookahead brickwall limiter (`0.4 ms` attack) catches inter-sample peaks and stops sudden shouts from clipping past your ceiling.
7. **Delay-Compensated Toggle:** Switching between **ENABLED** and **DISABLED** crossfades smoothly against a delay-matched dry signal with zero echo or timing jump.

---

## Controls

### Top Bar
| Element | What it does |
|---|---|
| **ENABLED / DISABLED** | Turns processing ON or OFF cleanly (compare processed voice vs raw mic). |
| **RESET** | Resets all sliders and switches back to defaults. |

### 1-Click Presets
- **Balanced:** General-purpose defaults (`-16 LUFS`, `-24 dB` silence floor, `100%` AI noise removal).
- **Discord:** Tight gate (`-30 dB`), `100%` AI noise removal, `-15 LUFS` target for voice chat.
- **Podcast:** Gentle gate (`-18 dB`), `85%` AI noise removal, `-16 LUFS` target for natural dynamics.
- **Stream:** Louder `-13 LUFS` target, faster leveling, `-0.5 dBFS` ceiling.

*(Presets keep your **Mic Input** routing setting untouched.)*

### Mic Input Routing
| Mode | When to use it |
|---|---|
| **Auto** | Default. Matches the host channel layout (best for standard 1-ch USB mics). |
| **L+R Mono** | Mixes stereo input equally into mono. |
| **Left -> Both** | Fixes 2-channel audio interfaces (e.g., Scarlett 2i2, UM2) where your mic is plugged into Input 1 (Left) and Channel 2 is silent. |
| **Right -> Both** | Uses Input 2 (Right) for both channels. |

### Sliders
- **Noise & Room Cleanup:** `Low-Cut Rumble Filter` (`20–160 Hz`), `AI Noise Removal` (`0–100%`), `Voice Gate Sensitivity` (`0–95%`), `Silence Reduction` (`-60` to `0 dB`).
- **Automatic Volume:** `Auto Volume` (`ON`/`OFF`), `Target Voice Volume` (`-28` to `-10 LUFS`), `Max Volume Boost` (`0` to `+30 dB`), `Leveling Speed` (`0–100%`).
- **Protection & Output:** `Clip Protection` (`-6.0` to `-0.1 dBFS`), `Final Output Trim` (`-12` to `+12 dB`).

*Tip: Drag any slider, scroll the mouse wheel to fine-tune, or double-click a slider to reset it. Keyboard navigation is also supported (`Tab` / `Shift+Tab` to move focus, `Enter` or `Space` to toggle, `Arrow` keys to adjust sliders, `Backspace` or `Delete` to reset).*

---

## Installation

Prebuilt binaries are on the GitHub [Releases](../../releases) page (not kept in the source tree).

1. Download `AutoGainNS-v*-win-x64.zip` from [Releases](../../releases) (or [build from source](#building-from-source)) and extract the **`AutoGainNS.vst3`** folder bundle (keep its internal `Contents\x86_64-win\` structure intact).
2. Copy the **`AutoGainNS.vst3`** folder into your VST3 directory (usually `C:\Program Files\Common Files\VST3\AutoGainNS.vst3`) and rescan plugins in your host.
3. For **system-wide Windows mic processing** (Discord, games, browsers), I personally load it with [EqualizerAPO-XT](https://github.com/115dkk/EqualizerAPO-XT) on my mic capture device (select `AutoGainNS.vst3\Contents\x86_64-win\AutoGainNS.vst3` in the file picker, using **Input Mono / Output Mono** for a 1-channel mic).

### Compatibility
Built against the standard Steinberg VST 3 SDK for 64-bit Windows (`Mono`, `Mono-to-Stereo`, and `Stereo`). It should work in any 64-bit Windows VST3 host, though full compatibility everywhere is not guaranteed; so far it has only been tested in [EqualizerAPO-XT](https://github.com/115dkk/EqualizerAPO-XT) and the included C++ test suite.

---

## Quick-Start by Microphone Setup

- **USB Microphone (Condenser / Dynamic):** Keep **Mic Input** on `Auto`, pick **Balanced**, **Discord**, or **Podcast**, and adjust **Voice Gate Sensitivity** if room noise bleeds between sentences.
- **2-Channel Audio Interface (Mic in Input 1, Input 2 empty):** If you only hear your mic on the left side, switch **Mic Input** to **`Left -> Both`** (or **`Right -> Both`** if plugged into Input 2).
- **Quiet Dynamic Mic (e.g., SM7B, PodMic without inline preamp):** Raise **Max Volume Boost** up to `+24` to `+30 dB` so **Auto Volume** can lift your voice cleanly to `-16 LUFS`.

---

## Notes & Limitations

- **64-bit Windows & 32-bit float only:** Currently targets Windows x64 and `kSample32` (`32-bit` float) VST3 audio streams.
- **Lightweight denoiser scope:** RNNoise is chosen for low CPU and `12 ms` latency. It cleans steady room noise, fans, hiss, and moderate clicks well, but isn't a heavy dereverb model for echoey rooms.

---

## Building from Source

### Prerequisites
- Windows 10 / 11 (x64)
- Git + Visual Studio 2022 or newer with **Desktop development with C++** (MSVC x64 tools)

### Fetch Dependencies & Build
```powershell
.\scripts\fetch_deps.ps1
.\build.ps1
```
Optional flag to skip running the test binary:
```powershell
.\build.ps1 -SkipTests
```
`build.ps1` finds `vcvars64.bat` via `vswhere.exe`, builds `out\AutoGainNS.vst3\Contents\x86_64-win\AutoGainNS.vst3`, checks dependencies with `dumpbin /dependents`, sets Windows Read & Execute permissions for standard and service-hosted audio hosts, and runs the 9-test C++ suite (`build\test\test_dsp.exe`).

---

## Project Structure

```text
AutoGainNS/
├── src/
│   ├── dsp/
│   │   ├── biquad.hpp           # 64-bit Butterworth HPF/LPF & ITU-R BS.1770-4 K-weighting
│   │   ├── rnnoise_wrapper.hpp  # Multi-rate RNNoise wrapper & per-sample VAD
│   │   └── voice_processor.hpp  # Real-time voice DSP pipeline
│   └── plugin/
│       ├── ids.hpp              # Parameter IDs, range helpers, safe state serialization
│       ├── processor.hpp/.cpp   # VST3 AudioEffect + bus routing
│       ├── controller.hpp/.cpp  # VST3 EditController + presets + parameter registration
│       ├── editor_view.hpp/.cpp # HiDPI Win32 GDI UI (IPlugViewContentScaleSupport)
│       ├── factory.cpp          # VST3 plugin factory (v1.1.0)
│       └── exports.def          # DLL entry points (InitDll, ExitDll, GetPluginFactory)
├── scripts/
│   └── fetch_deps.ps1           # Fetches Steinberg VST 3 SDK + Xiph RNNoise & weights (SHA-256 verified)
├── tests/
│   └── test_dsp.cpp             # Automated DSP, multi-rate, state & VST3 binary test suite
├── build.ps1                    # Self-contained MSVC build & test script
└── LICENSE
```

---

## License

[MIT](LICENSE)

## Credits

- [Xiph.Org RNNoise](https://github.com/xiph/rnnoise) (BSD-3-Clause; Jean-Marc Valin, Mozilla, Amazon, Xiph.Org Foundation, Mark Borgerding) for real-time neural noise suppression and Voice Activity Detection.
- [Steinberg VST 3 SDK](https://github.com/steinbergmedia/vst3sdk) (MIT; Steinberg Media Technologies GmbH) for VST 3 plugin interfaces, audio processor/controller base classes, and HiDPI view scaling.
- [ITU-R BS.1770-4](https://www.itu.int/rec/R-REC-BS.1770) for K-weighting filter coefficients and momentary LUFS loudness measurement used by Auto Volume.
- [Audio EQ Cookbook](https://webaudio.github.io/Audio-EQ-Cookbook/audio-eq-cookbook.html) by Robert Bristow-Johnson for 2nd-order Butterworth high-pass and low-pass biquad filter formulas.
