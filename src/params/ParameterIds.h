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

    // NOT an APVTS parameter: the currently loaded IR file's absolute path is
    // stored as a plain property directly on apvts.state (see
    // PluginProcessor::loadImpulseResponseFromFile/getStateInformation), so
    // it round-trips through session/preset state without needing a float
    // parameter to represent a file path. irFilePathBProperty is the
    // equivalent for IR B.
    inline constexpr auto irFilePathProperty = "irFilePath";
    inline constexpr auto irFilePathBProperty = "irFilePathB";
}
