#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "state/IrStateSerialization.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <random>

namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 512;

    // A realistic cabinet IR: enough taps and enough structure that a
    // round-trip losing precision, channel order, or length would show up.
    juce::AudioBuffer<float> makeIr (int numChannels, int numSamples, unsigned int seed)
    {
        std::mt19937 engine (seed);
        std::uniform_real_distribution<float> distribution (-1.0f, 1.0f);

        juce::AudioBuffer<float> buffer (numChannels, numSamples);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* data = buffer.getWritePointer (channel);

            for (int i = 0; i < numSamples; ++i)
            {
                const auto decay = std::exp (-6.0f * static_cast<float> (i) / static_cast<float> (numSamples));
                data[i] = distribution (engine) * decay;
            }

            // A channel-dependent leading tap, so a channel swap is visible.
            data[0] = 1.0f - 0.25f * static_cast<float> (channel);
        }

        return buffer;
    }

    // Renders a fixed sine through a processor and returns the result, so two
    // processors' outputs can be compared sample for sample.
    juce::AudioBuffer<float> render (NaveAudioProcessor& processor, int numBlocks = 8)
    {
        processor.prepareToPlay (testSampleRate, testBlockSize);

        juce::AudioBuffer<float> output (2, testBlockSize * numBlocks);
        juce::AudioBuffer<float> buffer (2, testBlockSize);
        juce::MidiBuffer midi;

        for (int blockIndex = 0; blockIndex < numBlocks; ++blockIndex)
        {
            TestHelpers::fillWithSine (buffer, testSampleRate, 440.0, 0.5f,
                                        static_cast<juce::int64> (blockIndex) * testBlockSize);

            processor.processBlock (buffer, midi);

            for (int channel = 0; channel < 2; ++channel)
                output.copyFrom (channel, blockIndex * testBlockSize, buffer, channel, 0, testBlockSize);
        }

        return output;
    }

    float maxDifferenceDb (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        const auto numSamples = juce::jmin (a.getNumSamples(), b.getNumSamples());

        float worst = 0.0f;

        for (int channel = 0; channel < juce::jmin (a.getNumChannels(), b.getNumChannels()); ++channel)
            for (int i = 0; i < numSamples; ++i)
                worst = juce::jmax (worst, std::abs (a.getSample (channel, i) - b.getSample (channel, i)));

        return juce::Decibels::gainToDecibels (worst, -200.0f);
    }
}

//==============================================================================
// The codec itself, before any of the plugin plumbing.
TEST_CASE ("IR audio blobs round-trip exactly", "[state][ir-embed]")
{
    SECTION ("mono and stereo IRs survive bit-exactly")
    {
        const auto numChannels = GENERATE (1, 2);

        CAPTURE (numChannels);

        const auto original = makeIr (numChannels, 2048, 7u);

        const auto blob = IrState::encodeImpulseResponse (original, testSampleRate);

        REQUIRE (blob.getSize() > 0);

        juce::AudioBuffer<float> restored;
        double restoredSampleRate = 0.0;

        REQUIRE (IrState::decodeImpulseResponse (blob, restored, restoredSampleRate));

        CHECK (restoredSampleRate == Catch::Approx (testSampleRate));
        REQUIRE (restored.getNumChannels() == numChannels);
        REQUIRE (restored.getNumSamples() == original.getNumSamples());

        // Float equality, deliberately: the format stores raw float32, so
        // anything short of bit-exact means a bug, not rounding.
        for (int channel = 0; channel < numChannels; ++channel)
            for (int i = 0; i < original.getNumSamples(); ++i)
                REQUIRE (restored.getSample (channel, i) == original.getSample (channel, i));
    }

    SECTION ("gzip actually shrinks a decaying IR")
    {
        const auto original = makeIr (1, 8192, 3u);

        const auto blob = IrState::encodeImpulseResponse (original, testSampleRate);
        const auto rawBytes = static_cast<size_t> (original.getNumSamples()) * sizeof (float);

        CAPTURE (blob.getSize(), rawBytes);

        // Size is the whole reason for the cap and the compression; if this
        // ever regressed to "bigger than raw" the session-bloat risk would be
        // real.
        CHECK (blob.getSize() < rawBytes);
    }

    SECTION ("IRs past the length cap are refused, so the slot stays path-only")
    {
        const auto tooLong = static_cast<int> (testSampleRate * (IrState::maximumEmbeddedSeconds + 1.0));

        juce::AudioBuffer<float> buffer (1, tooLong);
        buffer.clear();
        buffer.setSample (0, 0, 1.0f);

        CHECK (IrState::encodeImpulseResponse (buffer, testSampleRate).getSize() == 0);
    }

    SECTION ("corrupt, foreign and empty blobs are rejected rather than loaded as noise")
    {
        juce::AudioBuffer<float> restored;
        double restoredSampleRate = 0.0;

        CHECK_FALSE (IrState::decodeImpulseResponse ({}, restored, restoredSampleRate));

        juce::MemoryBlock notGzip ("this is not a gzip stream", 25);
        CHECK_FALSE (IrState::decodeImpulseResponse (notGzip, restored, restoredSampleRate));

        // A valid blob truncated halfway must fail rather than return half an
        // IR padded with silence.
        auto truncated = IrState::encodeImpulseResponse (makeIr (1, 1024, 5u), testSampleRate);
        REQUIRE (truncated.getSize() > 16);
        truncated.setSize (truncated.getSize() / 2, false);

        CHECK_FALSE (IrState::decodeImpulseResponse (truncated, restored, restoredSampleRate));
    }
}

//==============================================================================
// Test 10 (merge gate): the headline fix. A session must reproduce its IR with
// the source file gone - the exact scenario that silently emptied the cabinet
// in v0.2.
TEST_CASE ("A saved session restores its IR after the source file is deleted", "[state][ir-embed]")
{
    const auto irFile = juce::File::createTempFile (".wav");
    const auto irFileB = juce::File::createTempFile (".wav");

    const auto irA = makeIr (1, 1024, 21u);
    const auto irB = makeIr (1, 768, 22u);

    REQUIRE (TestHelpers::writeWavFile (irFile, irA, testSampleRate));
    REQUIRE (TestHelpers::writeWavFile (irFileB, irB, testSampleRate));

    juce::MemoryBlock savedState;
    juce::AudioBuffer<float> originalRender;

    {
        NaveAudioProcessor processor;
        processor.prepareToPlay (testSampleRate, testBlockSize);

        REQUIRE (processor.loadImpulseResponseFromFile (irFile));
        REQUIRE (processor.loadImpulseResponseFromFileB (irFileB));

        originalRender = render (processor);

        processor.getStateInformation (savedState);
        REQUIRE (savedState.getSize() > 0);
    }

    // The files disappear - moved project, different machine, cleaned-up
    // downloads folder. v0.2 would silently fall back to a delta IR here.
    irFile.deleteFile();
    irFileB.deleteFile();

    REQUIRE_FALSE (irFile.existsAsFile());
    REQUIRE_FALSE (irFileB.existsAsFile());

    NaveAudioProcessor restored;
    restored.prepareToPlay (testSampleRate, testBlockSize);
    restored.setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));

    const auto restoredRender = render (restored);

    const auto residualDb = maxDifferenceDb (originalRender, restoredRender);

    CAPTURE (residualDb);

    // Not "close enough" - the restored instance must be the same plugin.
    CHECK (residualDb < -80.0f);

    // The paths are still carried as metadata, so the UI can tell the user
    // which file the IR came from even though the audio no longer depends on it.
    CHECK (restored.getCurrentIrFilePath().isNotEmpty());
    CHECK (restored.getCurrentIrFilePathB().isNotEmpty());
}

TEST_CASE ("Saved state stamps the current schema version and embeds both slots", "[state][ir-embed]")
{
    const auto irFile = juce::File::createTempFile (".wav");
    REQUIRE (TestHelpers::writeWavFile (irFile, makeIr (1, 512, 31u), testSampleRate));

    NaveAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);

    juce::MemoryBlock savedState;

    SECTION ("with no user IR, no audio blob is written")
    {
        processor.getStateInformation (savedState);

        CHECK (IrState::readSchemaVersion (processor.apvts.state) == IrState::currentSchemaVersion);
        CHECK_FALSE (processor.apvts.state.hasProperty (ParamIDs::irAudioAProperty));
        CHECK_FALSE (processor.apvts.state.hasProperty (ParamIDs::irAudioBProperty));
    }

    SECTION ("loading then clearing an IR removes its blob again")
    {
        REQUIRE (processor.loadImpulseResponseFromFile (irFile));

        processor.getStateInformation (savedState);
        REQUIRE (processor.apvts.state.hasProperty (ParamIDs::irAudioAProperty));

        // Clearing must not leave the old audio behind, or reloading the
        // session would resurrect an IR the user deliberately removed.
        processor.loadDefaultImpulseResponse();
        processor.getStateInformation (savedState);

        CHECK_FALSE (processor.apvts.state.hasProperty (ParamIDs::irAudioAProperty));
    }

    irFile.deleteFile();
}

//==============================================================================
// Test 11 (merge gate): v1 -> v2 migration.
TEST_CASE ("A v1 state migrates to v2 with Legacy alignment and neutral defaults", "[state][migration]")
{
    // Build a genuine v1 state: a v0.3.0 state with the schema version and
    // every v0.3.0 parameter stripped out, which is exactly what a v0.2 XML
    // chunk looks like.
    juce::MemoryBlock v1State;

    {
        NaveAudioProcessor processor;
        processor.prepareToPlay (testSampleRate, testBlockSize);

        // Non-default v0.2 parameters, so the migration is shown to preserve
        // them rather than resetting everything.
        processor.apvts.getParameter (ParamIDs::loCut)
            ->setValueNotifyingHost (processor.apvts.getParameter (ParamIDs::loCut)->convertTo0to1 (120.0f));
        processor.apvts.getParameter (ParamIDs::irBlend)
            ->setValueNotifyingHost (processor.apvts.getParameter (ParamIDs::irBlend)->convertTo0to1 (50.0f));

        auto state = processor.apvts.copyState();

        state.removeProperty (ParamIDs::stateVersionProperty, nullptr);

        for (const auto* id : { ParamIDs::blendMode, ParamIDs::alignMode, ParamIDs::irBTrim,
                                 ParamIDs::irBPolarity, ParamIDs::irBDelay, ParamIDs::irGainMode,
                                 ParamIDs::irAMinPhase, ParamIDs::irBMinPhase, ParamIDs::distanceAir,
                                 ParamIDs::loCutSlope, ParamIDs::hiCutSlope })
        {
            for (int i = state.getNumChildren(); --i >= 0;)
                if (state.getChild (i).getProperty ("id").toString() == id)
                    state.removeChild (i, nullptr);
        }

        const std::unique_ptr<juce::XmlElement> xml (state.createXml());
        juce::AudioProcessor::copyXmlToBinary (*xml, v1State);
    }

    NaveAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);
    processor.setStateInformation (v1State.getData(), static_cast<int> (v1State.getSize()));

    const auto choiceIndex = [&] (const char* id)
    {
        auto* parameter = dynamic_cast<juce::AudioParameterChoice*> (processor.apvts.getParameter (id));
        REQUIRE (parameter != nullptr);
        return parameter->getIndex();
    };

    const auto boolValue = [&] (const char* id)
    {
        auto* parameter = dynamic_cast<juce::AudioParameterBool*> (processor.apvts.getParameter (id));
        REQUIRE (parameter != nullptr);
        return parameter->get();
    };

    SECTION ("alignMode is forced to Legacy so the session keeps v0.2's maths")
    {
        CHECK (choiceIndex (ParamIDs::alignMode) == 0);
    }

    SECTION ("every other new parameter stays neutral")
    {
        CHECK (choiceIndex (ParamIDs::blendMode) == 0);
        CHECK (choiceIndex (ParamIDs::irGainMode) == 0);
        CHECK (choiceIndex (ParamIDs::loCutSlope) == 0);
        CHECK (choiceIndex (ParamIDs::hiCutSlope) == 0);

        CHECK_FALSE (boolValue (ParamIDs::irBPolarity));
        CHECK_FALSE (boolValue (ParamIDs::irAMinPhase));
        CHECK_FALSE (boolValue (ParamIDs::irBMinPhase));
        CHECK_FALSE (boolValue (ParamIDs::distanceAir));

        // A margin, not exact equality: a plain value round-trips through the
        // parameter's normalised 0-1 representation, and 0.0 dB sits at
        // normalised 0.5, so it comes back a fraction of a microunit off. That
        // is the parameter system's float precision, not a non-neutral default.
        CHECK (processor.apvts.getRawParameterValue (ParamIDs::irBTrim)->load() == Catch::Approx (0.0f).margin (1.0e-4f));
        CHECK (processor.apvts.getRawParameterValue (ParamIDs::irBDelay)->load() == Catch::Approx (0.0f).margin (1.0e-4f));
    }

    SECTION ("the pre-existing parameters are preserved")
    {
        CHECK (processor.apvts.getRawParameterValue (ParamIDs::loCut)->load() == Catch::Approx (120.0f).margin (0.5f));
        CHECK (processor.apvts.getRawParameterValue (ParamIDs::irBlend)->load() == Catch::Approx (50.0f).margin (0.5f));
    }

    SECTION ("the state is stamped v2 so the migration does not run twice")
    {
        CHECK (IrState::readSchemaVersion (processor.apvts.state) == IrState::currentSchemaVersion);
    }
}

//==============================================================================
// Test 11b (merge gate): the blend-engaged migration null.
//
// This is the case tests 10/11 structurally cannot catch. Both of those use
// either delta IRs or a blend-disengaged state, so neither would notice a
// v0.3.0 change that introduced an internal timing offset in the wet path -
// which is precisely what the rejected centre-tap designs for IR B Delay would
// have done to every upgraded v0.2 blend session.
TEST_CASE ("A blend-engaged v1 session renders identically after migration", "[state][migration][merge-gate]")
{
    const auto irFile = juce::File::createTempFile (".wav");
    const auto irFileB = juce::File::createTempFile (".wav");

    // REAL, non-delta IRs in both slots - a delta pair would hide any timing
    // offset, since a delta convolved with anything is just a delay.
    REQUIRE (TestHelpers::writeWavFile (irFile, makeIr (1, 1024, 41u), testSampleRate));
    REQUIRE (TestHelpers::writeWavFile (irFileB, makeIr (1, 1024, 42u), testSampleRate));

    // The v0.2 "Even Blend" factory-preset parameter set: blend engaged at 50%.
    const auto configureAsV02Session = [&] (NaveAudioProcessor& processor)
    {
        const auto set = [&] (const char* id, float plainValue)
        {
            auto* parameter = processor.apvts.getParameter (id);
            REQUIRE (parameter != nullptr);
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (plainValue));
        };

        set (ParamIDs::irBlend, 50.0f);
        set (ParamIDs::mix, 100.0f);
        set (ParamIDs::level, 0.0f);
    };

    // The reference: a v0.2-equivalent render, i.e. Legacy alignment and every
    // v0.3.0 feature at its neutral default.
    juce::AudioBuffer<float> goldenRender;

    {
        NaveAudioProcessor processor;
        processor.prepareToPlay (testSampleRate, testBlockSize);

        configureAsV02Session (processor);

        auto* alignParameter = processor.apvts.getParameter (ParamIDs::alignMode);
        alignParameter->setValueNotifyingHost (alignParameter->convertTo0to1 (0.0f));  // Legacy

        REQUIRE (processor.loadImpulseResponseFromFile (irFile));
        REQUIRE (processor.loadImpulseResponseFromFileB (irFileB));

        goldenRender = render (processor);
    }

    // Now the upgraded session: a v1 chunk (no stateVersion, no v0.3.0
    // parameters) loaded into this build.
    juce::MemoryBlock v1State;

    {
        NaveAudioProcessor processor;
        processor.prepareToPlay (testSampleRate, testBlockSize);

        configureAsV02Session (processor);

        REQUIRE (processor.loadImpulseResponseFromFile (irFile));
        REQUIRE (processor.loadImpulseResponseFromFileB (irFileB));

        auto state = processor.apvts.copyState();
        state.removeProperty (ParamIDs::stateVersionProperty, nullptr);

        for (const auto* id : { ParamIDs::blendMode, ParamIDs::alignMode, ParamIDs::irBTrim,
                                 ParamIDs::irBPolarity, ParamIDs::irBDelay, ParamIDs::irGainMode,
                                 ParamIDs::irAMinPhase, ParamIDs::irBMinPhase, ParamIDs::distanceAir,
                                 ParamIDs::loCutSlope, ParamIDs::hiCutSlope })
        {
            for (int i = state.getNumChildren(); --i >= 0;)
                if (state.getChild (i).getProperty ("id").toString() == id)
                    state.removeChild (i, nullptr);
        }

        const std::unique_ptr<juce::XmlElement> xml (state.createXml());
        juce::AudioProcessor::copyXmlToBinary (*xml, v1State);
    }

    NaveAudioProcessor migrated;
    migrated.prepareToPlay (testSampleRate, testBlockSize);
    migrated.setStateInformation (v1State.getData(), static_cast<int> (v1State.getSize()));

    const auto migratedRender = render (migrated);

    const auto residualDb = maxDifferenceDb (goldenRender, migratedRender);

    CAPTURE (residualDb);

    // The guarantee: an upgraded blend-engaged session sounds exactly as it
    // did. Any internal offset in the wet path - a centre-tap delay, an
    // always-on delay line, an alignment change - would show up here.
    CHECK (residualDb < -80.0f);

    irFile.deleteFile();
    irFileB.deleteFile();
}

//==============================================================================
// Test 11b, second half: automating Blend across the engage epsilon must not
// step. This is where a design that engaged an internal offset "only while
// blend is engaged" would betray itself.
TEST_CASE ("Automating Blend across the engage epsilon is continuous", "[state][migration][merge-gate]")
{
    const auto irFile = juce::File::createTempFile (".wav");
    REQUIRE (TestHelpers::writeWavFile (irFile, makeIr (1, 512, 51u), testSampleRate));

    NaveAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);

    REQUIRE (processor.loadImpulseResponseFromFileB (irFile));

    auto* blendParameter = processor.apvts.getParameter (ParamIDs::irBlend);
    REQUIRE (blendParameter != nullptr);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    juce::MidiBuffer midi;

    constexpr int numBlocks = 60;

    float steadyStateStep = 0.0f;
    float crossingStep = 0.0f;

    for (int blockIndex = 0; blockIndex < numBlocks; ++blockIndex)
    {
        // Sweep Blend down through 0 and back up, crossing the engage epsilon
        // (0.001) twice.
        const auto phase = static_cast<float> (blockIndex) / static_cast<float> (numBlocks - 1);
        const auto blendPercent = std::abs (0.5f - phase) * 2.0f * 4.0f;

        blendParameter->setValueNotifyingHost (blendParameter->convertTo0to1 (blendPercent));

        TestHelpers::fillWithSine (buffer, testSampleRate, 1000.0, 0.5f,
                                    static_cast<juce::int64> (blockIndex) * testBlockSize);

        processor.processBlock (buffer, midi);

        float step = 0.0f;

        for (int i = 1; i < testBlockSize; ++i)
            step = juce::jmax (step, std::abs (buffer.getSample (0, i) - buffer.getSample (0, i - 1)));

        if (blockIndex < numBlocks / 2 - 4)
            steadyStateStep = juce::jmax (steadyStateStep, step);
        else if (blockIndex >= numBlocks / 2 - 4 && blockIndex <= numBlocks / 2 + 4)
            crossingStep = juce::jmax (crossingStep, step);
    }

    CAPTURE (steadyStateStep, crossingStep);

    REQUIRE (steadyStateStep > 0.0f);

    // No timing or level discontinuity at the crossing.
    CHECK (crossingStep < 3.0f * steadyStateStep);

    irFile.deleteFile();
}

//==============================================================================
// Test 12: the preset-apply path performs the same migration.
TEST_CASE ("Applying a preset without a schema version migrates it", "[state][migration][presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, testBlockSize);

    auto* alignParameter = dynamic_cast<juce::AudioParameterChoice*> (processor.apvts.getParameter (ParamIDs::alignMode));
    REQUIRE (alignParameter != nullptr);

    // Start from Precise, so a migration to Legacy is an observable change
    // rather than a coincidence.
    alignParameter->setValueNotifyingHost (alignParameter->convertTo0to1 (1.0f));
    REQUIRE (alignParameter->getIndex() == 1);

    // The factory "Default" preset ships with an explicit stateVersion, so it
    // must NOT migrate; a pre-v0.3.0 preset (no stateVersion) must.
    SECTION ("a versioned preset leaves alignment alone")
    {
        REQUIRE (processor.presetManager.loadPreset ("Default"));
        CHECK (alignParameter->getIndex() == 1);
    }

    SECTION ("an unversioned preset is migrated to Legacy")
    {
        // Drive the shared migration directly with the version the preset
        // system would report for a file lacking the field.
        IrState::migrateParametersIfNeeded (processor.apvts, IrState::legacySchemaVersion);

        CHECK (alignParameter->getIndex() == 0);
    }
}

//==============================================================================
// Factory presets use delta IRs, where alignment has nothing to align - so
// both modes must sound identical, and the migration is sonically a no-op.
TEST_CASE ("With delta IRs both alignment modes render identically", "[state][migration]")
{
    const auto renderWithAlignMode = [] (float alignIndex)
    {
        NaveAudioProcessor processor;
        processor.prepareToPlay (testSampleRate, testBlockSize);

        auto* parameter = processor.apvts.getParameter (ParamIDs::alignMode);
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (alignIndex));

        auto* blend = processor.apvts.getParameter (ParamIDs::irBlend);
        blend->setValueNotifyingHost (blend->convertTo0to1 (50.0f));

        return render (processor);
    };

    const auto legacyRender = renderWithAlignMode (0.0f);
    const auto preciseRender = renderWithAlignMode (1.0f);

    const auto residualDb = maxDifferenceDb (legacyRender, preciseRender);

    CAPTURE (residualDb);
    CHECK (residualDb < -80.0f);
}
