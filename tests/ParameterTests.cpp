#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "dsp/CabConvolutionEngine.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

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
            // v0.1/v0.2 - IDs frozen, see ParameterIds.h.
            ParamIDs::loCut, ParamIDs::hiCut, ParamIDs::irBlend, ParamIDs::micDistance, ParamIDs::mix, ParamIDs::level,
            // v0.3.0.
            ParamIDs::blendMode, ParamIDs::alignMode, ParamIDs::irBTrim, ParamIDs::irBPolarity,
            ParamIDs::irBDelay, ParamIDs::irGainMode, ParamIDs::irAMinPhase, ParamIDs::irBMinPhase,
            ParamIDs::distanceAir, ParamIDs::loCutSlope, ParamIDs::hiCutSlope,
        };

        for (const auto* id : allIds)
            CHECK (apvts.getParameter (id) != nullptr);
    }

    SECTION ("total parameter count matches the current layout")
    {
        // 6 from v0.1/v0.2 plus the 11 added in v0.3.0. This count is
        // deliberately hard-coded: bumping it should be a conscious act, since
        // adding a parameter changes every host's automation lane numbering.
        CHECK (apvts.processor.getParameters().size() == 17);
    }

    SECTION ("every v0.3.0 parameter defaults to its neutral value")
    {
        // The backward-compatibility contract in one place: a v0.2 session
        // carries none of these IDs, so each one loads at its default, and
        // every default here must be the value at which the corresponding
        // v0.3.0 code path is bypassed or an identity. The audible half of this
        // guarantee is pinned by the golden-render null tests in StateTests.
        const auto defaultChoiceIndex = [&] (const juce::String& id)
        {
            // Via the RangedAudioParameter base: getDefaultValue() is public
            // there, but private on the concrete AudioParameterChoice/Bool
            // subclasses in JUCE 8.
            auto* parameter = requireParam (apvts, id);
            return static_cast<int> (std::lround (
                parameter->convertFrom0to1 (parameter->getDefaultValue())));
        };

        const auto defaultBool = [&] (const juce::String& id)
        {
            auto* parameter = requireParam (apvts, id);
            return parameter->getDefaultValue() > 0.5f;
        };

        CHECK (defaultChoiceIndex (ParamIDs::blendMode) == 0);   // Crossfade, the v0.2 path
        CHECK (defaultChoiceIndex (ParamIDs::irGainMode) == 0);  // Energy, JUCE's normalisation
        CHECK (defaultChoiceIndex (ParamIDs::loCutSlope) == 0);  // 12 dB/oct, the v0.2 filter
        CHECK (defaultChoiceIndex (ParamIDs::hiCutSlope) == 0);  // 12 dB/oct

        // alignMode is the deliberate exception: fresh instances get the
        // better algorithm (Precise), and upgraded v0.2 sessions are pinned to
        // Legacy by the state migration instead of by this default.
        CHECK (defaultChoiceIndex (ParamIDs::alignMode) == 1);

        CHECK (! defaultBool (ParamIDs::irBPolarity));
        CHECK (! defaultBool (ParamIDs::irAMinPhase));
        CHECK (! defaultBool (ParamIDs::irBMinPhase));
        CHECK (! defaultBool (ParamIDs::distanceAir));

        checkFloatDefault (apvts, ParamIDs::irBTrim, 0.0f);
        checkFloatDefault (apvts, ParamIDs::irBDelay, 0.0f);

        checkFloatRange (apvts, ParamIDs::irBTrim, -24.0f, 24.0f);
        checkFloatRange (apvts, ParamIDs::irBDelay, -5.0f, 5.0f);
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
