#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "dsp/CabConvolutionEngine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
    // Convenience wrapper: fetches a parameter by ID and requires it to
    // exist before returning, so every SECTION below fails loudly (not with
    // a null-deref) if an ID typo ever creeps in.
    juce::RangedAudioParameter* requireParam (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    {
        auto* param = apvts.getParameter (id);
        REQUIRE (param != nullptr);
        return param;
    }

    // Checks that a float parameter's underlying NormalisableRange covers
    // [expectedMin, expectedMax], independent of any skew/log mapping.
    void checkFloatRange (juce::AudioProcessorValueTreeState& apvts,
                          const juce::String& id,
                          float expectedMin,
                          float expectedMax)
    {
        auto* param = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (id));
        REQUIRE (param != nullptr);

        const auto range = param->getNormalisableRange().getRange();
        CHECK (range.getStart() == Catch::Approx (expectedMin));
        CHECK (range.getEnd() == Catch::Approx (expectedMax));
    }

    // Checks a float parameter's default value in real (non-normalised)
    // units, going through convertTo0to1 so log-skewed ranges are handled
    // the same way as linear ones.
    void checkFloatDefault (juce::AudioProcessorValueTreeState& apvts,
                            const juce::String& id,
                            float expectedDefault)
    {
        auto* param = requireParam (apvts, id);
        CHECK (param->getDefaultValue() == Catch::Approx (param->convertTo0to1 (expectedDefault)).margin (1e-4));
    }
}

TEST_CASE ("Processor instantiates with the expected parameters", "[processor][parameters]")
{
    NaveAudioProcessor processor;
    auto& apvts = processor.apvts;

    SECTION ("plugin name")
    {
        CHECK (processor.getName() == juce::String ("Nave"));
    }

    SECTION ("all documented parameter IDs resolve")
    {
        static constexpr const char* allIds[] = {
            ParamIDs::loCut, ParamIDs::hiCut, ParamIDs::irBlend, ParamIDs::micDistance, ParamIDs::mix, ParamIDs::level,
        };

        for (const auto* id : allIds)
            CHECK (apvts.getParameter (id) != nullptr);
    }

    SECTION ("total parameter count matches the current layout")
    {
        CHECK (apvts.processor.getParameters().size() == 6);
    }

    SECTION ("IR Blend: defaults to IR A only (0%) and covers its documented range")
    {
        checkFloatDefault (apvts, ParamIDs::irBlend, 0.0f);
        checkFloatRange (apvts, ParamIDs::irBlend, 0.0f, 100.0f);
    }

    SECTION ("Distance: defaults to its minimum (the bypassed/off position) and covers its documented range")
    {
        checkFloatDefault (apvts, ParamIDs::micDistance, CabConvolutionEngine::distanceMinPercent);
        checkFloatRange (apvts, ParamIDs::micDistance, CabConvolutionEngine::distanceMinPercent, CabConvolutionEngine::distanceMaxPercent);
    }

    SECTION ("LoCut: defaults to its minimum (the bypassed/off position) and covers its documented range")
    {
        checkFloatDefault (apvts, ParamIDs::loCut, CabConvolutionEngine::loCutMinHz);
        checkFloatRange (apvts, ParamIDs::loCut, CabConvolutionEngine::loCutMinHz, CabConvolutionEngine::loCutMaxHz);
    }

    SECTION ("HiCut: defaults to its maximum (the bypassed/off position) and covers its documented range")
    {
        checkFloatDefault (apvts, ParamIDs::hiCut, CabConvolutionEngine::hiCutMaxHz);
        checkFloatRange (apvts, ParamIDs::hiCut, CabConvolutionEngine::hiCutMinHz, CabConvolutionEngine::hiCutMaxHz);
    }

    SECTION ("Mix: dry/wet defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::mix, 100.0f);
        checkFloatRange (apvts, ParamIDs::mix, 0.0f, 100.0f);
    }

    SECTION ("Level: output trim defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::level, 0.0f);
        checkFloatRange (apvts, ParamIDs::level, -24.0f, 24.0f);
    }

    SECTION ("No IR file path is set on a freshly constructed processor")
    {
        CHECK (processor.getCurrentIrFilePath().isEmpty());
        CHECK (processor.getCurrentIrFilePathB().isEmpty());
    }
}
