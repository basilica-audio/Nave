#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

// The complete Nave signal path, independent of juce::AudioProcessor so it
// can be exercised directly by unit tests without instantiating a full
// plugin (see tests/EngineTests.cpp). Owns all DSP state; every buffer/
// filter/convolution engine is allocated in prepare() and never reallocated
// on the audio thread.
//
// Signal flow (see docs/architecture.md for the full diagram and the
// latency-compensation rationale):
//
//   input -> [convolution: crossfade of IR A and IR B] -> Distance
//         -> LoCut HPF -> HiCut LPF -> Dry/Wet mix -> Level (output trim)
//         -> output
//
// With no user IR loaded, both convolution slots run a unit-impulse (delta)
// IR - mathematically a passthrough - so the plugin is a valid, transparent
// effect out of the box. juce::dsp::Convolution is constructed with its
// default (Latency{0}) configuration, i.e. the zero-latency uniformly
// partitioned algorithm, so CabConvolutionEngine reports zero latency
// whenever the loaded IR is short enough for that algorithm (true for the
// default delta IR and for any IR that fits within one FFT block).
//
// IR Blend crossfades between two independently loadable impulse responses
// (IR A, the original v0.1 slot, and IR B) - e.g. two different cabs, or two
// mic positions on the same cab. Blend defaults to 0% (IR A only), which is
// numerically identical to the v0.1 single-IR signal path. Loading IR B
// applies "inter-IR phase alignment" (see IrAlignment.h) beforehand, so the
// two IRs' transient onsets line up before they're ever summed.
//
// Distance emulates the effect of mic-to-cab distance: a gentle proximity-
// effect low-shelf cut plus a high-shelf "air absorption" cut, both scaling
// with the Distance parameter. Distance defaults to 0% ("off"), the same
// explicit-bypass-at-the-extreme pattern used by LoCut/HiCut below.
class CabConvolutionEngine
{
public:
    CabConvolutionEngine();

    // LoCut/HiCut range boundaries. LoCut's minimum (its default) and
    // HiCut's maximum (its default) are each treated as an explicit "off"
    // position: process() skips that filter's IIR processing entirely
    // rather than merely setting an extreme-but-still-active cutoff, so the
    // plugin's default state is a true bit-accurate passthrough (see
    // docs/architecture.md, "Filter bypass at the range extremes", and
    // tests/EngineTests.cpp's null test).
    static constexpr float loCutMinHz = 20.0f;
    static constexpr float loCutMaxHz = 800.0f;
    static constexpr float hiCutMinHz = 2000.0f;
    static constexpr float hiCutMaxHz = 20000.0f;

    // Distance range boundaries. Distance's minimum (its default, 0%) is
    // treated the same way as LoCut/HiCut's bypass extremes above: an
    // explicit "off" position where process() skips the distance-emulation
    // filters entirely, so the default state stays a true passthrough.
    static constexpr float distanceMinPercent = 0.0f;
    static constexpr float distanceMaxPercent = 100.0f;

    // Allocates all DSP state. Must be called (and completed) before the
    // first process() call, and again whenever sample rate/block size/
    // channel count change. Not real-time safe - allocates and may take
    // noticeable time to rebuild the convolution engine.
    void prepare (const juce::dsp::ProcessSpec& spec);

    // Clears all filter/convolution/delay-line state without deallocating.
    // Safe to call from the audio thread (e.g. on playback stop/loop).
    void reset();

    // Processes `block` in place. `block` must have at most the maximum
    // sample/channel counts declared to prepare(); a zero-sample block is a
    // safe no-op. No allocation occurs here.
    void process (juce::dsp::AudioBlock<float>& block);

    // Parameter setters, in real units (Hz, dB, 0-1 proportion). Safe to
    // call every block from the audio thread - no allocation/locks. LoCut/
    // HiCut are smoothed internally and re-applied once per block; Level is
    // smoothed by the underlying juce::dsp::Gain ramp; Mix is smoothed
    // internally (see process()).
    void setLoCutHz (float newFrequencyHz);
    void setHiCutHz (float newFrequencyHz);
    void setMixProportion (float newProportion01);
    void setLevelDb (float newLevelDb);

    // IR Blend: 0 = IR A only (the v0.1 default signal path, bit-identical),
    // 1 = IR B only. Smoothed internally like Mix; safe to call every block
    // from the audio thread.
    void setBlendProportion (float newProportion01);

    // Distance: 0% (default) = off/bypassed, 100% = maximum simulated
    // mic-to-cab distance coloration. Smoothed internally like LoCut/HiCut;
    // safe to call every block from the audio thread.
    void setDistancePercent (float newDistancePercent);

    // Loads a new impulse response into slot A (the original/primary IR).
    // MUST be called off the audio thread (e.g. the message thread, in
    // response to a user picking a file) - reading/building `irBuffer`
    // involves file I/O and allocation that has already happened by the
    // time this is called, but juce::dsp::Convolution::loadImpulseResponse()
    // itself is documented as wait-free, so this call is safe even if it
    // were ever invoked from the audio thread. `irBuffer` is moved from
    // (Convolution takes ownership to avoid an audio-thread copy). Also
    // records this IR's onset sample/rate as the reference that a
    // subsequently loaded IR B is phase-aligned against (see
    // setImpulseResponseB()).
    void setImpulseResponse (juce::AudioBuffer<float> irBuffer, double irSampleRate);

    // Resets slot A to the default unit-impulse (delta) IR - a mathematical
    // passthrough. Used both for the plugin's out-of-the-box default and to
    // let the user explicitly clear a loaded IR. Same off-audio-thread
    // contract as setImpulseResponse(). Also resets the phase-alignment
    // reference back to the delta IR's trivial (sample 0) onset.
    void loadDefaultImpulseResponse();

    // Loads a new impulse response into slot B (the secondary IR used for IR
    // Blend). Same off-audio-thread contract as setImpulseResponse(). Before
    // loading, `irBuffer` is time-shifted ("inter-IR phase alignment", see
    // IrAlignment.h) so its detected onset lines up with slot A's most
    // recently recorded onset - this prevents comb-filtering when Blend
    // crossfades the two convolution outputs together.
    void setImpulseResponseB (juce::AudioBuffer<float> irBuffer, double irSampleRate);

    // Resets slot B to the default unit-impulse (delta) IR. Same
    // off-audio-thread contract as loadDefaultImpulseResponse().
    void loadDefaultImpulseResponseB();

    // Convolution engine latency in samples, valid after prepare() has run.
    // Zero for the default zero-latency convolution configuration this
    // engine always uses.
    int getLatencySamples() const noexcept { return latencySamples; }

private:
    static constexpr double smoothingTimeSeconds = 0.05;
    // Butterworth (maximally-flat) Q for both the LoCut and HiCut filters.
    static constexpr float filterQ = juce::MathConstants<float>::sqrt2 / 2.0f;
    // Tolerance around the range extremes within which LoCut/HiCut are
    // treated as fully "off" (bypassed). Comfortably larger than any
    // floating-point rounding from parameter smoothing, comfortably smaller
    // than any musically meaningful step within the parameter's range.
    static constexpr float bypassEpsilonHz = 0.5f;
    // Same idea for Blend (guards against float noise landing exactly at
    // 0%, which would otherwise flip blendEngaged on and off every block)
    // and Distance.
    static constexpr float blendBypassEpsilon = 0.001f;
    static constexpr float distanceBypassEpsilonPercent = 0.5f;

    // Distance emulation: fixed shelf frequencies, gain scaling linearly
    // (in dB) with the normalised Distance parameter. Deliberately gentle -
    // this approximates the two most audible effects of mic-to-cab distance
    // (reduced proximity-effect bass buildup, and high-frequency air
    // absorption/off-axis darkening), not a physically exact model.
    static constexpr float distanceLowShelfFrequencyHz = 200.0f;
    static constexpr float distanceLowShelfMaxCutDb = -6.0f;
    static constexpr float distanceHighShelfFrequencyHz = 5000.0f;
    static constexpr float distanceHighShelfMaxCutDb = -9.0f;

    double sampleRate = 44100.0;
    int numChannelsPrepared = 2;

    juce::dsp::Convolution convolution;
    juce::dsp::Convolution convolutionB;

    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> loCutFilter;
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> hiCutFilter;
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> distanceLowShelfFilter;
    juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Coefficients<float>> distanceHighShelfFilter;

    juce::dsp::Gain<float> outputLevel;

    // Sized generously above any realistic convolution latency (zero for the
    // default configuration this engine uses, but architected generically)
    // so setWetLatency() never exceeds the mixer's internal delay-line
    // capacity regardless of sample rate.
    juce::dsp::DryWetMixer<float> dryWetMixer { 1024 };

    // Scratch storage for the IR B convolution branch when Blend is
    // engaged, sized to (numChannelsPrepared x maximumBlockSize) in
    // prepare() and never resized in process() - see process()'s
    // scratchLargeEnough guard for the defensive fallback if a host ever
    // sends a block larger than promised.
    juce::AudioBuffer<float> scratchBuffer;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> loCutFrequencySmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> hiCutFrequencySmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> mixSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> blendSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> distanceSmoothed;

    // Last commanded values (ParameterLayout defaults until a setter is
    // called), re-applied on every prepare() so re-prepare (sample-rate
    // change, etc.) never resets a live parameter back to a default or lets
    // a smoother start from an invalid 0 Hz.
    float lastLoCutHz = loCutMinHz;
    float lastHiCutHz = hiCutMaxHz;
    float lastMixProportion = 1.0f;
    float lastBlendProportion = 0.0f;
    float lastDistancePercent = distanceMinPercent;
    // juce::dsp::Gain's internal SmoothedValue defaults to a *linear* gain of
    // 0 (silence) until setGainDecibels() is called at least once - unlike
    // LoCut/HiCut/Mix above, there is no engine-internal notion of a "neutral"
    // Level, so it must be primed explicitly in prepare() or a freshly
    // constructed engine would be silent, not a passthrough.
    float lastLevelDb = 0.0f;

    int latencySamples = 0;

    // True once setImpulseResponse()/loadDefaultImpulseResponse() (slot A)
    // has been called at least once; guards prepare() from redundantly
    // reloading the default IR on every re-prepare (juce::dsp::Convolution
    // automatically retains and re-resamples the most recently loaded IR -
    // see prepare()). anyImpulseResponseBLoaded is the equivalent guard for
    // slot B.
    bool anyImpulseResponseLoaded = false;
    bool anyImpulseResponseBLoaded = false;

    // Previous block's engaged (i.e. not bypassed) state for LoCut/HiCut/
    // Distance, used to detect bypassed->engaged transitions so the
    // filter(s) can be reset to a clean state exactly then (see process()).
    bool loCutEngagedPreviously = false;
    bool hiCutEngagedPreviously = false;
    bool distanceEngagedPreviously = false;

    // IR A's most recently loaded onset sample/rate, recorded by
    // setImpulseResponse()/loadDefaultImpulseResponse() and used as the
    // reference that setImpulseResponseB() phase-aligns IR B against (see
    // IrAlignment.h).
    int lastIrAOnsetSample = 0;
    double lastIrASampleRate = 44100.0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CabConvolutionEngine)
};
