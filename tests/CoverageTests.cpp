#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "dsp/CabConvolutionEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <random>

// Broadened Catch2 coverage for M1: sample-rate sweeps (44.1-192 kHz),
// extreme parameter automation, mono/stereo bus configurations, and a
// long-run NaN/Inf stability soak test. Complements (does not replace) the
// focused null/reference/latency/state tests in the other test files.
namespace
{
    void setParam (NaveAudioProcessor& processor, const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }

    // The full sample-rate sweep the M1 spec calls for. 44100/48000 are the
    // common tracking rates; 88200/96000/176400/192000 cover the
    // high-resolution range up to 192 kHz.
    constexpr std::array<double, 6> sweepSampleRates { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
}

TEST_CASE ("Null test holds across the full 44.1-192 kHz sample-rate sweep", "[coverage][sample-rate][null]")
{
    for (const auto rate : sweepSampleRates)
    {
        CAPTURE (rate);

        constexpr int blockSize = 1024;

        CabConvolutionEngine engine;
        engine.setLoCutHz (CabConvolutionEngine::loCutMinHz);
        engine.setHiCutHz (CabConvolutionEngine::hiCutMaxHz);
        engine.setMixProportion (1.0f);
        engine.setLevelDb (0.0f);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = rate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        engine.prepare (spec);

        REQUIRE (engine.getLatencySamples() == 0);

        juce::AudioBuffer<float> reference (2, blockSize);
        TestHelpers::fillWithSine (reference, rate, 1000.0, 0.5f);

        juce::AudioBuffer<float> processed;
        processed.makeCopyOf (reference);

        juce::dsp::AudioBlock<float> block (processed);
        engine.process (block);

        float maxResidual = 0.0f;

        for (int channel = 0; channel < reference.getNumChannels(); ++channel)
        {
            const auto* refData = reference.getReadPointer (channel);
            const auto* outData = processed.getReadPointer (channel);

            for (int i = 0; i < blockSize; ++i)
                maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));
        }

        CHECK (maxResidual < 1.0e-4f);
    }
}

TEST_CASE ("LoCut/HiCut engaged produce finite, sane output across the sample-rate sweep", "[coverage][sample-rate]")
{
    for (const auto rate : sweepSampleRates)
    {
        CAPTURE (rate);

        constexpr int blockSize = 512;

        NaveAudioProcessor processor;
        processor.prepareToPlay (rate, blockSize);

        setParam (processor, ParamIDs::loCut, 300.0f);
        setParam (processor, ParamIDs::hiCut, 5000.0f);
        setParam (processor, ParamIDs::mix, 100.0f);

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        for (int block = 0; block < 4; ++block)
        {
            TestHelpers::fillWithSine (buffer, rate, 440.0, 0.7f);
            CHECK_NOTHROW (processor.processBlock (buffer, midi));
            CHECK (TestHelpers::allSamplesFinite (buffer));
        }

        CHECK (processor.getLatencySamples() == 0);
    }
}

TEST_CASE ("Mono bus layout processes without NaN/Inf and stays a passthrough at defaults", "[coverage][bus-layout]")
{
    NaveAudioProcessor processor;

    juce::AudioProcessor::BusesLayout monoLayout;
    monoLayout.inputBuses.add (juce::AudioChannelSet::mono());
    monoLayout.outputBuses.add (juce::AudioChannelSet::mono());

    REQUIRE (processor.checkBusesLayoutSupported (monoLayout));
    REQUIRE (processor.setBusesLayout (monoLayout));

    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> reference (1, 512);
    TestHelpers::fillWithSine (reference, 48000.0, 1000.0, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::MidiBuffer midi;
    processor.processBlock (processed, midi);

    CHECK (TestHelpers::allSamplesFinite (processed));

    float maxResidual = 0.0f;
    const auto* refData = reference.getReadPointer (0);
    const auto* outData = processed.getReadPointer (0);

    for (int i = 0; i < 512; ++i)
        maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));

    CHECK (maxResidual < 1.0e-4f);
}

TEST_CASE ("Mono bus layout with engaged filters and a loaded IR produces no NaN/Inf", "[coverage][bus-layout]")
{
    NaveAudioProcessor processor;

    juce::AudioProcessor::BusesLayout monoLayout;
    monoLayout.inputBuses.add (juce::AudioChannelSet::mono());
    monoLayout.outputBuses.add (juce::AudioChannelSet::mono());

    REQUIRE (processor.setBusesLayout (monoLayout));

    processor.prepareToPlay (48000.0, 512);

    const auto irFile = juce::File::createTempFile (".wav");
    juce::AudioBuffer<float> ir (1, 64);

    for (int i = 0; i < ir.getNumSamples(); ++i)
        ir.setSample (0, i, std::exp (-0.05f * static_cast<float> (i)));

    REQUIRE (TestHelpers::writeWavFile (irFile, ir, 48000.0));
    REQUIRE (processor.loadImpulseResponseFromFile (irFile));

    setParam (processor, ParamIDs::loCut, 250.0f);
    setParam (processor, ParamIDs::hiCut, 6000.0f);
    setParam (processor, ParamIDs::micDistance, 50.0f);
    setParam (processor, ParamIDs::mix, 80.0f);

    juce::AudioBuffer<float> buffer (1, 512);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        TestHelpers::fillWithSine (buffer, 48000.0, 300.0 + static_cast<double> (block) * 200.0, 0.8f);
        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }

    irFile.deleteFile();
}

TEST_CASE ("Stereo bus layout (explicit re-assertion) processes without NaN/Inf", "[coverage][bus-layout]")
{
    NaveAudioProcessor processor;

    juce::AudioProcessor::BusesLayout stereoLayout;
    stereoLayout.inputBuses.add (juce::AudioChannelSet::stereo());
    stereoLayout.outputBuses.add (juce::AudioChannelSet::stereo());

    REQUIRE (processor.checkBusesLayoutSupported (stereoLayout));
    REQUIRE (processor.setBusesLayout (stereoLayout));

    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::irBlend, 40.0f);
    setParam (processor, ParamIDs::micDistance, 30.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    for (int block = 0; block < 4; ++block)
    {
        TestHelpers::fillWithSine (buffer, 48000.0, 220.0, 0.6f);
        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("Unsupported bus layouts (3+ channels) are rejected", "[coverage][bus-layout]")
{
    NaveAudioProcessor processor;

    juce::AudioProcessor::BusesLayout surroundLayout;
    surroundLayout.inputBuses.add (juce::AudioChannelSet::createLCR());
    surroundLayout.outputBuses.add (juce::AudioChannelSet::createLCR());

    CHECK_FALSE (processor.checkBusesLayoutSupported (surroundLayout));
}

TEST_CASE ("Long-run stability: 2000 blocks of continuous automated processing produce no NaN/Inf", "[coverage][stability]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);

    std::mt19937 rng (42);
    std::uniform_real_distribution<float> unit (0.0f, 1.0f);

    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;

    constexpr int numBlocks = 2000; // ~10.7 s of audio at 48 kHz/256 samples

    for (int block = 0; block < numBlocks; ++block)
    {
        // Sweep every parameter continuously across its full range so the
        // filters/mixer/gain smoothers are perpetually chasing a moving
        // target - the scenario most likely to accumulate state drift or
        // denormal/NaN issues over a long run.
        const auto phase = static_cast<float> (block) / static_cast<float> (numBlocks);

        setParam (processor, ParamIDs::loCut,
                  CabConvolutionEngine::loCutMinHz + phase * (CabConvolutionEngine::loCutMaxHz - CabConvolutionEngine::loCutMinHz));
        setParam (processor, ParamIDs::hiCut,
                  CabConvolutionEngine::hiCutMaxHz - phase * (CabConvolutionEngine::hiCutMaxHz - CabConvolutionEngine::hiCutMinHz));
        setParam (processor, ParamIDs::irBlend, unit (rng) * 100.0f);
        setParam (processor, ParamIDs::micDistance, unit (rng) * 100.0f);
        setParam (processor, ParamIDs::mix, unit (rng) * 100.0f);
        setParam (processor, ParamIDs::level, -24.0f + unit (rng) * 48.0f);

        TestHelpers::fillWithSine (buffer, 48000.0, 100.0 + unit (rng) * 8000.0, 0.7f);

        processor.processBlock (buffer, midi);

        if (! TestHelpers::allSamplesFinite (buffer))
        {
            FAIL ("Non-finite sample produced at block " << block);
            break;
        }
    }

    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Long-run stability with a loaded IR and IR B blend produces no NaN/Inf", "[coverage][stability]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (44100.0, 512);

    const auto irFileA = juce::File::createTempFile (".wav");
    const auto irFileB = juce::File::createTempFile (".wav");

    juce::AudioBuffer<float> irA (2, 128);
    juce::AudioBuffer<float> irB (2, 96);

    std::mt19937 rng (7);
    std::uniform_real_distribution<float> unit (-1.0f, 1.0f);

    for (int channel = 0; channel < 2; ++channel)
    {
        for (int i = 0; i < irA.getNumSamples(); ++i)
            irA.setSample (channel, i, unit (rng) * std::exp (-0.03f * static_cast<float> (i)));

        for (int i = 0; i < irB.getNumSamples(); ++i)
            irB.setSample (channel, i, unit (rng) * std::exp (-0.05f * static_cast<float> (i)));
    }

    REQUIRE (TestHelpers::writeWavFile (irFileA, irA, 44100.0));
    REQUIRE (TestHelpers::writeWavFile (irFileB, irB, 44100.0));
    REQUIRE (processor.loadImpulseResponseFromFile (irFileA));
    REQUIRE (processor.loadImpulseResponseFromFileB (irFileB));

    std::uniform_real_distribution<float> unit01 (0.0f, 1.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    constexpr int numBlocks = 300; // ~3.5 s of audio at 44.1 kHz/512 samples

    for (int block = 0; block < numBlocks; ++block)
    {
        setParam (processor, ParamIDs::irBlend, unit01 (rng) * 100.0f);
        setParam (processor, ParamIDs::micDistance, unit01 (rng) * 100.0f);

        TestHelpers::fillWithSine (buffer, 44100.0, 150.0 + unit01 (rng) * 6000.0, 0.6f);

        processor.processBlock (buffer, midi);
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }

    irFileA.deleteFile();
    irFileB.deleteFile();
}
