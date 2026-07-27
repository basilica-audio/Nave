#pragma once

// Central definition of all AudioProcessorValueTreeState parameter IDs for
// Nave. See docs/architecture.md for the corresponding signal-flow diagram.
//
// FROZEN AS OF THE v0.1 PARAMETER LAYOUT:
// Parameter IDs below must NEVER change once shipped - saved sessions and
// presets persist the APVTS state keyed by these string IDs, and renaming or
// removing one would silently break every user's saved state. Ranges,
// defaults, and skew MAY still be refined during voicing/tuning milestones;
// only the IDs themselves are frozen.
namespace ParamIDs
{
    // Post-convolution high-pass. Range spans its own "off" position at the
    // minimum (20 Hz, the default): CabConvolutionEngine bypasses the filter
    // entirely rather than merely setting a subsonic cutoff, so the default
    // state is a true passthrough (see docs/architecture.md, "Filter bypass
    // at the range extremes").
    inline constexpr auto loCut = "loCut";

    // Post-convolution low-pass. Symmetric to loCut: its "off" position is
    // its maximum (20 kHz, the default), also a true bypass.
    inline constexpr auto hiCut = "hiCut";

    // Dry/wet mix. Default 100% (fully wet) - a cabinet IR is normally run
    // fully in the signal path.
    inline constexpr auto mix = "mix";

    // Output trim, applied after the dry/wet mix.
    inline constexpr auto level = "level";

    // IR Blend: crossfades between IR A (the original slot, loaded via
    // "Load IR...") and IR B (loaded via "Load IR B..."). Default 0% (IR A
    // only) is numerically identical to the v0.1 single-IR signal path, so
    // adding this parameter doesn't change any existing preset's sound.
    inline constexpr auto irBlend = "irBlend";

    // Distance: simulated mic-to-cab distance. Default 0% is an explicit
    // "off" position (see CabConvolutionEngine's bypass-at-the-extreme
    // pattern), so adding this parameter doesn't change any existing
    // preset's sound either.
    inline constexpr auto micDistance = "micDistance";

    //==========================================================================
    // v0.3.0 additions ("First-Class Cab Engine"). Every one of these defaults
    // to a neutral value, so a v0.2 session or preset loaded into v0.3.0
    // renders bit-identically (pinned by tests/StateTests.cpp's migration
    // nulls). Same frozen-ID contract as the six above.

    // Blend Mode: {Crossfade, Morph}. Crossfade (default) is the v0.2
    // parallel-convolver crossfade. Morph replaces it with a single convolver
    // running a minimum-phase + bulk-delay interpolation of IR A and IR B, so
    // intermediate blend values are a true mic-position morph instead of a
    // comb-filtered sum (see docs/architecture.md).
    inline constexpr auto blendMode = "blendMode";

    // IR Align: {Legacy, Precise}. Legacy is v0.2's relative-threshold onset
    // detector plus an integer shift. Precise is FFT cross-correlation with
    // parabolic sub-sample refinement, fractional-delay application and
    // automatic polarity detection. Fresh instances default to Precise; the
    // v1 -> v2 state migration explicitly writes Legacy so upgraded sessions
    // keep their exact v0.2 alignment maths.
    inline constexpr auto alignMode = "alignMode";

    // IR B Trim: post-convolution gain on the IR-B branch only, in dB.
    inline constexpr auto irBTrim = "irBTrim";

    // IR B Polarity: inverts the IR-B branch's signal (10 ms clickless
    // crossfade between +1 and -1), independent of the automatic polarity
    // flip alignment may apply to the IR buffer itself.
    inline constexpr auto irBPolarity = "irBPolarity";

    // IR B Delay: inter-slot timing offset in milliseconds, +/-5 ms. Realised
    // as a dual-sided branch delay with no internal offset (B branch delays
    // max(d, 0), A branch delays max(-d, 0)) so the neutral 0 ms setting adds
    // no latency to either branch - see CabConvolutionEngine.h.
    inline constexpr auto irBDelay = "irBDelay";

    // IR Gain Match: {Energy, Loudness}. Energy (default) is JUCE's
    // Convolution::Normalise::yes energy normalisation, i.e. v0.2 behaviour.
    // Loudness pre-scales each IR by its ITU-R BS.1770 K-weighted energy
    // instead, so switching IRs is level-fair to the ear rather than to a
    // flat-weighted integral.
    inline constexpr auto irGainMode = "irGainMode";

    // Per-slot minimum-phase transform. Converts the stored raw IR to its
    // minimum-phase equivalent (same magnitude response, energy front-loaded)
    // off the audio thread, which makes IRs captured by different vendors mix
    // without phase cancellation.
    inline constexpr auto irAMinPhase = "irAMinPhase";
    inline constexpr auto irBMinPhase = "irBMinPhase";

    // Distance Air: adds the time-of-flight pre-delay of the simulated
    // mic-to-cab distance (2.9 ms at 100%) to the wet path, on top of the
    // Distance shelves' tonal model. Off by default.
    inline constexpr auto distanceAir = "distanceAir";

    // LoCut/HiCut filter slope: {12 dB/oct, 24 dB/oct}. 12 (default) is the
    // single 2nd-order Butterworth section v0.2 shipped; 24 cascades two
    // sections with the 4th-order Butterworth Q pair.
    inline constexpr auto loCutSlope = "loCutSlope";
    inline constexpr auto hiCutSlope = "hiCutSlope";

    //==========================================================================
    // NOT an APVTS parameter: the currently loaded IR file's absolute path is
    // stored as a plain property directly on apvts.state (see
    // PluginProcessor::loadImpulseResponseFromFile/getStateInformation), so
    // it round-trips through session/preset state without needing a float
    // parameter to represent a file path. irFilePathBProperty is the
    // equivalent for IR B.
    inline constexpr auto irFilePathProperty = "irFilePath";
    inline constexpr auto irFilePathBProperty = "irFilePathB";

    // v0.3.0 state schema v2 (see src/state/IrStateSerialization.h). Also not
    // APVTS parameters: an int schema version stamped on apvts.state, and the
    // two gzip'd embedded IR audio blobs that make a saved session
    // self-contained instead of dependent on the IR files still existing at
    // their original paths.
    inline constexpr auto stateVersionProperty = "stateVersion";
    inline constexpr auto irAudioAProperty = "irAudioA";
    inline constexpr auto irAudioBProperty = "irAudioB";
}
