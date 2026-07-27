#pragma once

#include "FractionalDelay.h"
#include "IrAlignment.h"
#include "MorphEngine.h"

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
// Distance emulates the effect of mic-to-cab distance: a proximity-effect
// low-shelf cut plus a high-shelf darkening cut (driven far more by
// loudspeaker directivity than literal air absorption at reamping distances
// - see docs/research-notes.md SS2), both scaling with the Distance
// parameter. Distance defaults to 0% ("off"), the same explicit-bypass-at-
// the-extreme pattern used by LoCut/HiCut below.
//
// v0.2.0 (design-brief.md, "Distance" module spec): the low-shelf's gain no
// longer scales linearly against the normalised Distance value. Real
// proximity effect is front-loaded - "accelerates exponentially... then
// saturates" (docs/research-notes.md SS1) - so the low-shelf now scales
// against normalisedDistance raised to distanceLowShelfTaperExponent (>1),
// which concentrates most of the audible cut in the first third or so of
// the knob's travel and tapers off toward 100%. The high-shelf intentionally
// keeps its plain-linear taper: Two Notes' own reference model attributes
// off-axis darkening to a *separate* axis (their Center control) from
// distance, so Nave's single-knob high-shelf is already a simplification of
// that other axis, not the front-loaded proximity effect - a linear taper
// there is the honest choice, not an oversight (see design-brief.md).
// v0.3.0 additions, in signal-flow order:
//
//   input -> [convolution: Crossfade of IR A/IR B, or a single Morph IR]
//         -> IR B branch Trim/Polarity, dual-sided branch Delay
//         -> Distance Air (time-of-flight pre-delay) -> Distance shelves
//         -> LoCut (12 or 24 dB/oct) -> HiCut (12 or 24 dB/oct)
//         -> Dry/Wet mix -> Level
//
// Every one of them is neutral at its default, so a v0.2 session renders
// bit-identically (see src/params/ParameterLayout.cpp and the golden-render
// null tests in tests/StateTests.cpp).
//
// THREADING. The setters split into two groups, and the split is load-bearing:
//
//   * Audio-thread-safe (callable every block): the continuous and cheap
//     controls - LoCut/HiCut/Mix/Level/Blend/Distance, plus blend mode, IR B
//     trim/polarity/delay, Distance Air and the two filter slopes. None of
//     them allocate, lock, or touch impulse-response content.
//
//   * Message-thread only: the four that change what is actually IN a
//     convolver - align mode, IR gain mode, and the two per-slot minimum-phase
//     switches. Each re-runs FFT-scale analysis and reloads the stock
//     convolution engines, which is neither cheap nor real-time safe.
//     NaveAudioProcessor routes these through an AsyncUpdater rather than
//     calling them from processBlock().
//
// CLICK POLICY (binding, v0.3.0). juce::dsp::Convolution has no crossfade
// hook: loadImpulseResponse() resets the engine, which is audible. Making the
// three message-thread switches above click-free would need duplicate stock
// instances per slot plus fade plumbing - deliberately out of scope for this
// release. They are rare, stepped, deliberate configuration changes, and v0.2
// already hard-swaps audibly on every IR load, so v0.3.0 accepts the same
// brief reset on them (documented in docs/manual.md). Click-free IR exchange
// is guaranteed on the Morph path only, where MorphConvolver crossfades two
// filter sets over a shared input history. Everything continuous - Blend, Mix,
// Trim, Polarity, Delay, Distance, and the 12<->24 dB/oct slope switch - stays
// click-free via audio-thread gains, delays and chain crossfades.
class CabConvolutionEngine
{
public:
    CabConvolutionEngine();
    ~CabConvolutionEngine();

    // How IR A and IR B are combined.
    enum class BlendMode
    {
        // v0.2 behaviour: two convolvers in parallel, their outputs
        // crossfaded. Simple and predictable, but intermediate positions comb
        // wherever the two IRs' arrivals differ.
        Crossfade,

        // A single convolver running a minimum-phase + bulk-delay
        // interpolation of the two IRs - a true mic-position morph, with
        // nothing to comb against. See MorphEngine.h.
        Morph
    };

    // How a loaded IR is scaled.
    enum class GainMode
    {
        // JUCE's Convolution::Normalise::yes - a flat-weighted energy
        // reference. Bit-identical to v0.2.
        Energy,

        // ITU-R BS.1770 K-weighted energy, so two IRs that measure the same
        // also *sound* equally loud. See IrLoudness.h.
        Loudness
    };

    // Filter slope for LoCut/HiCut.
    enum class Slope
    {
        // A single 2nd-order Butterworth section - what v0.2 shipped.
        TwelveDbPerOctave,

        // Two cascaded 2nd-order sections with the 4th-order Butterworth Q
        // pair, i.e. a 4th-order Butterworth response.
        TwentyFourDbPerOctave
    };

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
    // setImpulseResponseB()). If a real (non-default) IR B is already
    // loaded, its alignment is re-run against this new reference before
    // returning, so a later A reload never leaves IR B silently aligned to a
    // stale onset (see #13).
    void setImpulseResponse (juce::AudioBuffer<float> irBuffer, double irSampleRate);

    // Resets slot A to the default unit-impulse (delta) IR - a mathematical
    // passthrough. Used both for the plugin's out-of-the-box default and to
    // let the user explicitly clear a loaded IR. Same off-audio-thread
    // contract as setImpulseResponse(). Also resets the phase-alignment
    // reference back to the delta IR's trivial (sample 0) onset, and
    // (like setImpulseResponse() above) re-aligns an already-loaded IR B
    // against it (see #13).
    void loadDefaultImpulseResponse();

    // Loads a new impulse response into slot B (the secondary IR used for IR
    // Blend). Same off-audio-thread contract as setImpulseResponse(). Before
    // loading, `irBuffer` is time-shifted ("inter-IR phase alignment", see
    // IrAlignment.h) so its detected onset lines up with slot A's most
    // recently recorded onset - this prevents comb-filtering when Blend
    // crossfades the two convolution outputs together. A copy of the raw,
    // pre-alignment buffer is retained so a later IR A reload can re-run
    // this alignment automatically (see setImpulseResponse() and #13).
    void setImpulseResponseB (juce::AudioBuffer<float> irBuffer, double irSampleRate);

    // Resets slot B to the default unit-impulse (delta) IR. Same
    // off-audio-thread contract as loadDefaultImpulseResponse(). Also clears
    // the retained raw-IR-B/alignment bookkeeping above, since the default
    // delta IR is never re-aligned on a subsequent IR A reload.
    void loadDefaultImpulseResponseB();

    // Convolution engine latency in samples, valid after prepare() has run.
    // Zero for the default zero-latency convolution configuration this
    // engine always uses. Every v0.3.0 delay is a wet-path *effect* (Distance
    // Air, IR B Delay, the morph's bulk delay), not processing latency, so
    // none of them are reported here or compensated by the host.
    int getLatencySamples() const noexcept { return latencySamples; }

    //==========================================================================
    // v0.3.0 audio-thread-safe setters. Safe to call every block: no
    // allocation, no locks, no impulse-response content touched.

    // Crossfade (default) or Morph. Switching crossfades the two path outputs
    // over 50 ms - both run during the fade - so the change is audible in
    // character but not as a click.
    void setBlendMode (BlendMode newMode) noexcept;

    // IR B branch output trim in dB. Sample-accurate ramp; 0 dB is unity.
    void setIrBTrimDb (float newTrimDb) noexcept;

    // Inverts the IR B branch. Crossfaded between +1 and -1 over 10 ms, so it
    // is clickless. Independent of the automatic polarity flip Precise
    // alignment may apply to the IR buffer itself off-thread.
    void setIrBPolarityInverted (bool shouldInvert) noexcept;

    // Inter-slot timing offset in milliseconds, +/-5 ms.
    //
    // Realised as a DUAL-SIDED branch delay with no internal offset: the IR B
    // branch delays by max(d, 0) and the IR A branch by max(-d, 0), so both
    // delay times are always >= 0 and both processors are skipped entirely at
    // d = 0. That is what makes the neutral setting bit-identical to v0.2 for
    // every session, including blend-engaged ones, and what keeps the mapping
    // continuous through zero (max() is continuous, so automating across 0
    // glides proportionally instead of lurching).
    //
    // Deliberately rejected alternatives, recorded so they are not
    // reintroduced: (a) a constant 5 ms centre-tap offset on both branches
    // while blend is engaged - this delays the entire wet path of every
    // upgraded v0.2 blend session by 5 ms, combing against the dry path at
    // Mix < 100% and stepping 5 ms whenever blend crosses the engage epsilon;
    // (b) engaging that offset only when d != 0 - a 5 ms pitch glide on first
    // knob touch. True negative *absolute* B timing without delaying A would
    // need the IR B buffer shifted earlier off-thread (IrAlignment::
    // shiftBySamples), which is out of scope for v0.3.0.
    void setIrBDelayMs (float newDelayMs) noexcept;

    // Adds the time-of-flight pre-delay of the simulated mic distance to the
    // wet path (2.9 ms at Distance 100%), on top of the Distance shelves'
    // tonal model. Off by default; at Distance 0% the whole Distance block
    // keeps its existing explicit bypass regardless.
    void setDistanceAirEnabled (bool shouldEnable) noexcept;

    // LoCut/HiCut slope. Switching crossfades two independently primed filter
    // chains over 10 ms, so there is no state-jump click.
    void setLoCutSlope (Slope newSlope) noexcept;
    void setHiCutSlope (Slope newSlope) noexcept;

    // Mono-in/stereo-out support: duplicates channel 0 into channel 1 at the
    // top of the chain, so a guitarist's mono DI fills a stereo bus instead of
    // playing out of one side. Set from prepareToPlay() once the host's bus
    // layout is known.
    void setDuplicateFirstChannel (bool shouldDuplicate) noexcept;

    //==========================================================================
    // v0.3.0 MESSAGE-THREAD-ONLY setters. Each re-analyses and reloads the
    // stock convolution engines (see the class-level click policy). Never call
    // these from processBlock().

    // Legacy (v0.2 onset heuristic) or Precise (cross-correlation with
    // sub-sample refinement and polarity detection). Re-aligns IR B.
    void setAlignMode (IrAlignment::Mode newMode);

    // Energy (v0.2, JUCE's normalisation) or Loudness (K-weighted). Reloads
    // both slots.
    void setGainMode (GainMode newMode);

    // Per-slot minimum-phase transform. Never destructive: the raw IR is
    // retained, so switching back restores the original exactly.
    void setIrAMinPhase (bool shouldApply);
    void setIrBMinPhase (bool shouldApply);

    //==========================================================================
    // The longer of the two loaded IRs, in seconds, so the host can size its
    // tail/render correctly instead of truncating a decaying cabinet.
    // Zero when both slots hold the default delta IR.
    double getTailLengthSeconds() const noexcept;

    // The raw (pre-alignment, pre-min-phase, pre-normalisation) IR buffers and
    // their sample rates, as loaded. These are what gets embedded in the
    // plugin state, so a saved session reproduces the same IR regardless of
    // which processing switches were on when it was saved.
    const juce::AudioBuffer<float>& getRawImpulseResponse (int slotIndex) const noexcept;
    double getRawImpulseResponseSampleRate (int slotIndex) const noexcept;
    bool hasUserImpulseResponse (int slotIndex) const noexcept;

private:
    using Duplicator = juce::dsp::ProcessorDuplicator<juce::dsp::IIR::Filter<float>,
                                                       juce::dsp::IIR::Coefficients<float>>;

    // A LoCut or HiCut filter that can be either 12 or 24 dB/oct, and can
    // cross from one to the other without a click.
    //
    // Both configurations are kept as SEPARATE filter objects rather than
    // reconfiguring one in place: a 24 dB/oct cascade's first section uses a
    // different Q from the 12 dB/oct section, so during a slope change the two
    // responses must exist simultaneously for their outputs to be crossfaded.
    // Reconfiguring in place would jump the IIR state instead - the click this
    // exists to avoid.
    struct CutFilterChain
    {
        Duplicator twelve;        // single 2nd-order section
        Duplicator twentyFourA;   // 4th-order Butterworth, section 1
        Duplicator twentyFourB;   // 4th-order Butterworth, section 2

        Slope activeSlope = Slope::TwelveDbPerOctave;
        Slope previousSlope = Slope::TwelveDbPerOctave;
        int crossfadeSamplesRemaining = 0;

        void prepare (const juce::dsp::ProcessSpec& spec);
        void reset();
    };

    // The two Q values of a 4th-order Butterworth, i.e. the poles' distance
    // from the unit circle for each cascaded section. Standard analog
    // prototype values; using 1/sqrt(2) twice instead would give a
    // Linkwitz-Riley response (-6 dB at cutoff), not Butterworth (-3 dB), and
    // would not match the 12 dB/oct path's gain at the corner.
    static constexpr float butterworth4thOrderQ1 = 0.54119610f;
    static constexpr float butterworth4thOrderQ2 = 1.30656296f;

    // How long a 12 <-> 24 dB/oct slope change takes to cross, and how long an
    // IR B polarity flip takes. Both are short enough to feel instant and long
    // enough to avoid a step discontinuity.
    static constexpr double slopeCrossfadeSeconds = 0.01;
    static constexpr double polarityCrossfadeSeconds = 0.01;

    // How long a Crossfade <-> Morph switch takes. Longer than the two above
    // because the two paths can differ substantially in character, so the
    // transition is a deliberate blend rather than a quick swap.
    static constexpr double blendModeCrossfadeSeconds = 0.05;

    // Filter coefficients are recomputed at this cadence rather than once per
    // block, so a fast filter sweep steps 32 samples at a time instead of up
    // to 1024 (survey gap #8). Small enough to be inaudible, large enough that
    // the trig in the coefficient formulas stays a rounding error in the CPU
    // budget.
    static constexpr int coefficientUpdateInterval = 32;

    // Time of flight for the Distance Air pre-delay: 2.9 ms at Distance 100%,
    // i.e. sound travelling one metre at ~343 m/s. Distance is a normalised
    // "how far back is the mic" control, and 100% is defined as a one-metre
    // pull-out from the cabinet.
    static constexpr float distanceAirMaxMilliseconds = 2.9f;

    // The IR B Delay control's range, and the delay-line capacity it implies.
    static constexpr float irBDelayMaxMilliseconds = 5.0f;

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

    // Distance emulation: fixed shelf frequencies. Deliberately gentle -
    // this approximates the two most audible effects of mic-to-cab distance
    // (reduced proximity-effect bass buildup, and high-frequency
    // directivity-driven darkening), not a physically exact model.
    static constexpr float distanceLowShelfFrequencyHz = 200.0f;
    static constexpr float distanceLowShelfMaxCutDb = -6.0f;
    static constexpr float distanceHighShelfFrequencyHz = 5000.0f;
    static constexpr float distanceHighShelfMaxCutDb = -9.0f;

    // v0.2.0 taper exponent for the low-shelf only (see the class-level
    // v0.2.0 comment above and the `tapered()` helper in
    // CabConvolutionEngine.cpp for the exact curve). Chosen from the
    // design-brief's sourced "^1.6-^2" range and tuned by ear against the
    // qualitative "accelerates then saturates" reference curve - see
    // design-brief.md's Honesty section for what this number is and isn't
    // calibrated against.
    //
    // Implementation note: a plain pow(normalisedDistance, exponent) with
    // exponent > 1 is *convex* on [0, 1] (slow start, accelerating near 1) -
    // that is a back-loaded curve, the opposite of what the brief specifies
    // ("most of the audible change happens in the first third of the knob's
    // travel, tapering off toward 100%"). `tapered()` instead applies the
    // exponent to the *complement* and inverts
    // (1 - (1 - normalisedDistance)^exponent, the standard "ease-out" power
    // curve), which is concave - a fast initial rise that flattens out
    // approaching 100%, the genuinely front-loaded shape the brief describes
    // and tests/EngineTests.cpp's taper-shape test asserts. The high-shelf
    // deliberately has no equivalent constant - it keeps a plain-linear
    // taper (see the class-level comment).
    static constexpr float distanceLowShelfTaperExponent = 1.8f;

    double sampleRate = 44100.0;
    int numChannelsPrepared = 2;

    // Internal helpers. All of these are message-thread only.
    void applySlot (int slotIndex);
    void applySlotA();
    void applySlotB();
    juce::AudioBuffer<float> prepareSlotForLoading (const juce::AudioBuffer<float>& raw,
                                                     double rawSampleRate,
                                                     bool applyMinPhase) const;
    void loadIntoConvolver (juce::dsp::Convolution& convolver,
                             juce::AudioBuffer<float> buffer,
                             double bufferSampleRate);

    // Audio-thread helpers.
    void updateCutCoefficients (bool loCutBypassed, bool hiCutBypassed, float loCutHz, float hiCutHz) noexcept;
    void updateDistanceCoefficients (float distancePercent) noexcept;
    void processCutFilter (CutFilterChain& chain,
                            juce::dsp::AudioBlock<float>& block,
                            int numSamples) noexcept;

    juce::dsp::Convolution convolution;
    juce::dsp::Convolution convolutionB;

    // The Morph path's own convolver + worker (blendMode == Morph). Runs
    // alongside the stock pair only while a mode crossfade is in flight.
    MorphEngine morphEngine;

    CutFilterChain loCutFilter;
    CutFilterChain hiCutFilter;

    Duplicator distanceLowShelfFilter;
    Duplicator distanceHighShelfFilter;

    // Wet-path fractional delays. All three are skipped entirely when their
    // delay is zero, so none of them costs anything - or changes anything - at
    // the default settings.
    FractionalDelay irBBranchDelay;   // IR B branch, max(irBDelay, 0)
    FractionalDelay irABranchDelay;   // IR A branch, max(-irBDelay, 0)
    FractionalDelay distanceAirDelay; // whole wet path, time of flight

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

    // Scratch for the slope crossfade (the not-yet-active chain's output) and
    // for the Crossfade <-> Morph path crossfade. Both sized in prepare() and
    // never resized on the audio thread.
    juce::AudioBuffer<float> slopeScratchBuffer;
    juce::AudioBuffer<float> morphScratchBuffer;

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
    // Distance/Blend, used to detect bypassed->engaged transitions so the
    // filter(s)/convolution engine can be reset to a clean state exactly
    // then (see process()). Blend's counterpart is convolutionB: unlike
    // LoCut/HiCut/Distance's IIR filters, convolutionB is a stateful
    // juce::dsp::Convolution whose internal overlap-add buffer keeps
    // accumulating output for future blocks even after the engine stops
    // calling its process() (see the Blend disengaged-branch below) - left
    // unreset, that stale, time-decoupled tail gets added back into the
    // output on re-engagement (see #12).
    bool loCutEngagedPreviously = false;
    bool hiCutEngagedPreviously = false;
    bool distanceEngagedPreviously = false;
    bool blendEngagedPreviously = false;

    // IR A's most recently loaded onset sample/rate, recorded by
    // setImpulseResponse()/loadDefaultImpulseResponse() and used as the
    // reference that setImpulseResponseB() phase-aligns IR B against (see
    // IrAlignment.h).
    int lastIrAOnsetSample = 0;
    double lastIrASampleRate = 44100.0;

    // A copy of IR B's raw, pre-alignment buffer/sample rate, retained so
    // that a later reload of IR A (which changes the alignment reference
    // above) can re-run alignment for the *already-loaded* IR B without
    // requiring the caller to reload it - see setImpulseResponse()/
    // loadDefaultImpulseResponse() and #13. irBNeedsAlignment is true once a
    // real (non-default) IR B has been loaded via setImpulseResponseB();
    // loadDefaultImpulseResponseB() clears it, since the default delta IR
    // has no meaningful onset to keep aligned.
    juce::AudioBuffer<float> lastIrBRawBuffer;
    double lastIrBRawSampleRate = 44100.0;
    bool irBNeedsAlignment = false;

    //==========================================================================
    // v0.3.0 state.

    // Slot A's raw buffer, retained for the same reasons slot B's already was
    // (re-applying min-phase, re-normalising on a gain-mode change) plus one
    // new one: it is what gets embedded in the saved session, and it is the
    // reference Precise alignment correlates IR B against.
    juce::AudioBuffer<float> lastIrARawBuffer;
    double lastIrARawSampleRate = 44100.0;
    bool hasUserIrA = false;
    bool hasUserIrB = false;

    // The post-processing IR A actually loaded into the convolver, kept as the
    // alignment reference so IR B is correlated against what is really
    // playing, not against the raw file.
    juce::AudioBuffer<float> alignmentReferenceBuffer;

    // Loaded IR durations, for getTailLengthSeconds().
    double irALengthSeconds = 0.0;
    double irBLengthSeconds = 0.0;

    // Message-thread configuration.
    IrAlignment::Mode alignMode = IrAlignment::Mode::Precise;
    GainMode gainMode = GainMode::Energy;
    bool irAMinPhaseEnabled = false;
    bool irBMinPhaseEnabled = false;

    // Audio-thread configuration.
    BlendMode blendMode = BlendMode::Crossfade;
    bool distanceAirEnabled = false;
    bool duplicateFirstChannel = false;

    float lastIrBTrimDb = 0.0f;
    bool lastIrBPolarityInverted = false;
    float lastIrBDelayMs = 0.0f;

    // Sample-accurate gain smoothers. Blend and Trim advance per sample in the
    // inner loops rather than being stepped once per block, which is what
    // removes the zipper noise a 1024-sample block used to produce on a fast
    // move (survey gap #8). At a static setting they converge to the same
    // constant v0.2 used, so nothing changes when nothing is moving.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> irBTrimGainSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> irBPolarityGainSmoothed;

    // 0 = fully Crossfade path, 1 = fully Morph path. Both paths run while
    // this is strictly between the two.
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> morphMixSmoothed;

    // Sample counter for the coefficientUpdateInterval sub-block cadence,
    // carried across blocks so the cadence does not reset at every boundary.
    int samplesSinceCoefficientUpdate = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CabConvolutionEngine)
};
