#include "ParameterLayout.h"
#include "ParameterIds.h"

#include "../dsp/CabConvolutionEngine.h"

namespace
{
    // True logarithmic (base-10) mapping for frequency parameters, so slider/
    // knob travel spends equal space per octave rather than per Hz. Uses
    // juce::mapToLog10/mapFromLog10 rather than NormalisableRange's built-in
    // power-law skew, which only approximates a log curve.
    juce::NormalisableRange<float> makeLogFrequencyRange (float minHz, float maxHz)
    {
        return juce::NormalisableRange<float> (
            minHz,
            maxHz,
            [] (float rangeStart, float rangeEnd, float normalised)
            { return juce::mapToLog10 (normalised, rangeStart, rangeEnd); },
            [] (float rangeStart, float rangeEnd, float value)
            { return juce::mapFromLog10 (value, rangeStart, rangeEnd); });
    }
}

namespace nave
{
    juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        //======================================================================
        // LoCut: post-convolution high-pass, 20 Hz - 800 Hz, default 20 Hz.
        // The default is the range minimum, which CabConvolutionEngine treats
        // as an explicit "off" (bypassed) position - see
        // CabConvolutionEngine.h for the bypass contract.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::loCut, 1 },
            "LoCut",
            makeLogFrequencyRange (CabConvolutionEngine::loCutMinHz, CabConvolutionEngine::loCutMaxHz),
            CabConvolutionEngine::loCutMinHz,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));

        //======================================================================
        // HiCut: post-convolution low-pass, 2 kHz - 20 kHz, default 20 kHz.
        // Symmetric to LoCut: the default is the range maximum, also an
        // explicit "off" position.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::hiCut, 1 },
            "HiCut",
            makeLogFrequencyRange (CabConvolutionEngine::hiCutMinHz, CabConvolutionEngine::hiCutMaxHz),
            CabConvolutionEngine::hiCutMaxHz,
            juce::AudioParameterFloatAttributes().withLabel ("Hz")));

        //======================================================================
        // IR Blend: crossfades between IR A and IR B. Default 0% (IR A only)
        // is bit-identical to the v0.1 single-IR signal path.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::irBlend, 1 },
            "IR Blend",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        //======================================================================
        // Distance: simulated mic-to-cab distance. The default (its range
        // minimum) is CabConvolutionEngine's explicit "off" position - see
        // CabConvolutionEngine.h for the bypass contract.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::micDistance, 1 },
            "Distance",
            juce::NormalisableRange<float> (CabConvolutionEngine::distanceMinPercent, CabConvolutionEngine::distanceMaxPercent, 0.1f),
            CabConvolutionEngine::distanceMinPercent,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        //======================================================================
        // Mix: dry/wet. Default 100% (fully wet) - a cabinet IR is normally
        // run fully in the signal path, not blended with the raw DI.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::mix, 1 },
            "Mix",
            juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f),
            100.0f,
            juce::AudioParameterFloatAttributes().withLabel ("%")));

        //======================================================================
        // Level: output trim.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::level, 1 },
            "Level",
            juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));

        //======================================================================
        // v0.3.0 parameters. Every default below is deliberately the *neutral*
        // value - the one at which the corresponding v0.3.0 code path is
        // either skipped entirely or mathematically an identity - so that a
        // v0.2 session or preset loaded into this build renders bit-identically
        // (tests/StateTests.cpp pins this with an -80 dBFS null against a v0.2
        // golden render, including a blend-engaged session).

        // Blend Mode: Crossfade (default) is exactly the v0.2 parallel-
        // convolver crossfade. Morph is the v0.3.0 min-phase + bulk-delay
        // interpolation path.
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { ParamIDs::blendMode, 1 },
            "Blend Mode",
            juce::StringArray { "Crossfade", "Morph" },
            0));

        // IR Align: Precise is the better algorithm and therefore the default
        // for *fresh* instances. Upgraded v0.2 sessions are explicitly given
        // Legacy by the v1 -> v2 state migration (see
        // src/state/IrStateSerialization.h), which is what keeps this new
        // default from changing how an existing session sounds.
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { ParamIDs::alignMode, 1 },
            "IR Align",
            juce::StringArray { "Legacy", "Precise" },
            1));

        // IR B Trim: 0 dB is unity, i.e. the v0.2 branch gain.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::irBTrim, 1 },
            "IR B Trim",
            juce::NormalisableRange<float> (-24.0f, 24.0f, 0.01f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("dB")));

        // IR B Polarity: off is +1, i.e. the v0.2 branch polarity.
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::irBPolarity, 1 },
            "IR B Polarity",
            false));

        // IR B Delay: 0 ms skips both branch delay processors entirely (see
        // CabConvolutionEngine.h's dual-sided delay contract), so neither
        // branch acquires any offset at the default.
        layout.add (std::make_unique<juce::AudioParameterFloat> (
            juce::ParameterID { ParamIDs::irBDelay, 1 },
            "IR B Delay",
            juce::NormalisableRange<float> (-5.0f, 5.0f, 0.01f),
            0.0f,
            juce::AudioParameterFloatAttributes().withLabel ("ms")));

        // IR Gain Match: Energy reproduces v0.2's Normalise::yes exactly.
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { ParamIDs::irGainMode, 1 },
            "IR Gain Match",
            juce::StringArray { "Energy", "Loudness" },
            0));

        // Per-slot minimum-phase transform: off leaves the raw IR untouched.
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::irAMinPhase, 1 },
            "IR A Min-Phase",
            false));

        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::irBMinPhase, 1 },
            "IR B Min-Phase",
            false));

        // Distance Air: off adds no wet pre-delay at all (the delay processor
        // is skipped, not merely set to zero time).
        layout.add (std::make_unique<juce::AudioParameterBool> (
            juce::ParameterID { ParamIDs::distanceAir, 1 },
            "Distance Air",
            false));

        // LoCut/HiCut slope: 12 dB/oct is the single 2nd-order section v0.2
        // shipped, so the default output is bit-identical.
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { ParamIDs::loCutSlope, 1 },
            "LoCut Slope",
            juce::StringArray { "12 dB/oct", "24 dB/oct" },
            0));

        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { ParamIDs::hiCutSlope, 1 },
            "HiCut Slope",
            juce::StringArray { "12 dB/oct", "24 dB/oct" },
            0));

        return layout;
    }
}
