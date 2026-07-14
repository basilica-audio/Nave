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

        return layout;
    }
}
