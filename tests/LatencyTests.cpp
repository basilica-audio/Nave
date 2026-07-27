#include "PluginProcessor.h"
#include "dsp/CabConvolutionEngine.h"
#include "params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("getLatencySamples() is zero after prepareToPlay with the default delta IR", "[latency]")
{
    NaveAudioProcessor processor;

    // Before prepareToPlay, no engine has been prepared yet - JUCE's default
    // AudioProcessor latency is 0.
    CHECK (processor.getLatencySamples() == 0);

    processor.prepareToPlay (48000.0, 512);

    // Cross-check against a standalone engine prepared identically: the
    // processor must report exactly what the engine (i.e. the convolution)
    // computes, not an approximation of it.
    CabConvolutionEngine referenceEngine;
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512;
    spec.numChannels = 2;
    referenceEngine.prepare (spec);

    CHECK (processor.getLatencySamples() == referenceEngine.getLatencySamples());

    // The default zero-latency Convolution configuration with a 1-sample
    // delta IR always reports zero latency.
    CHECK (processor.getLatencySamples() == 0);
}

TEST_CASE ("Latency is stable across repeated prepareToPlay calls at the same sample rate", "[latency]")
{
    NaveAudioProcessor processor;

    processor.prepareToPlay (44100.0, 256);
    const auto firstLatency = processor.getLatencySamples();

    processor.prepareToPlay (44100.0, 256);
    const auto secondLatency = processor.getLatencySamples();

    CHECK (firstLatency == secondLatency);
    CHECK (firstLatency == 0);
}

TEST_CASE ("Latency remains zero when the sample rate changes", "[latency]")
{
    NaveAudioProcessor processor;

    processor.prepareToPlay (44100.0, 512);
    const auto latencyAt44k = processor.getLatencySamples();

    processor.prepareToPlay (96000.0, 512);
    const auto latencyAt96k = processor.getLatencySamples();

    CHECK (latencyAt44k == 0);
    CHECK (latencyAt96k == 0);
}

TEST_CASE ("Latency remains zero after loading a custom (short) IR", "[latency]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> ir (1, 16);
    for (int i = 0; i < ir.getNumSamples(); ++i)
        ir.setSample (0, i, i == 0 ? 1.0f : 0.0f);

    CabConvolutionEngine engine;
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512;
    spec.numChannels = 2;
    engine.prepare (spec);
    engine.setImpulseResponse (std::move (ir), 48000.0);

    // Re-prepare after loading, mirroring how the processor's next
    // prepareToPlay() call would pick up a newly-loaded IR's latency.
    engine.prepare (spec);

    CHECK (engine.getLatencySamples() == 0);
}

TEST_CASE ("Latency remains zero with IR B loaded and Blend engaged", "[latency]")
{
    CabConvolutionEngine engine;
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512;
    spec.numChannels = 2;
    engine.prepare (spec);

    juce::AudioBuffer<float> irA (1, 16);
    juce::AudioBuffer<float> irB (1, 16);

    for (int i = 0; i < 16; ++i)
    {
        irA.setSample (0, i, i == 0 ? 1.0f : 0.0f);
        irB.setSample (0, i, i == 2 ? 0.5f : 0.0f);
    }

    engine.setImpulseResponse (std::move (irA), 48000.0);
    engine.setImpulseResponseB (std::move (irB), 48000.0);
    engine.setBlendProportion (0.5f);

    // Re-prepare so both loads are guaranteed active.
    engine.prepare (spec);

    CHECK (engine.getLatencySamples() == 0);
}

TEST_CASE ("Latency remains zero with Distance engaged", "[latency]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto* distanceParam = processor.apvts.getParameter (ParamIDs::micDistance);
    REQUIRE (distanceParam != nullptr);
    distanceParam->setValueNotifyingHost (distanceParam->convertTo0to1 (CabConvolutionEngine::distanceMaxPercent));

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;
    processor.processBlock (buffer, midi);

    CHECK (processor.getLatencySamples() == 0);
}

//==============================================================================
// Test 20 (v0.3.0): latency must stay exactly zero in EVERY new configuration.
//
// This matters more than it looks. All three v0.3.0 delays - the morph's bulk
// delay, IR B Delay, and Distance Air - are wet-path EFFECTS, not processing
// latency. Reporting them would make the host delay-compensate the whole track,
// shifting it against everything else in the session; the user asked for a
// delayed cabinet, not a delayed guitar part. Conversely, if a future change
// ever did introduce real latency, this is where it has to be declared.
TEST_CASE ("Latency stays zero across every v0.3.0 configuration", "[processor][latency][v030]")
{
    NaveAudioProcessor processor;

    auto* alignParameter = processor.apvts.getParameter (ParamIDs::alignMode);
    auto* blendModeParameter = processor.apvts.getParameter (ParamIDs::blendMode);
    auto* airParameter = processor.apvts.getParameter (ParamIDs::distanceAir);
    auto* loSlopeParameter = processor.apvts.getParameter (ParamIDs::loCutSlope);
    auto* hiSlopeParameter = processor.apvts.getParameter (ParamIDs::hiCutSlope);
    auto* minPhaseAParameter = processor.apvts.getParameter (ParamIDs::irAMinPhase);
    auto* minPhaseBParameter = processor.apvts.getParameter (ParamIDs::irBMinPhase);
    auto* gainModeParameter = processor.apvts.getParameter (ParamIDs::irGainMode);
    auto* delayParameter = processor.apvts.getParameter (ParamIDs::irBDelay);
    auto* distanceParameter = processor.apvts.getParameter (ParamIDs::micDistance);

    REQUIRE (alignParameter != nullptr);
    REQUIRE (blendModeParameter != nullptr);
    REQUIRE (airParameter != nullptr);

    // Everything at once: morph on, air on, both slopes at 24 dB/oct, both
    // min-phase switches on, loudness matching on, IR B delayed, distance up.
    blendModeParameter->setValueNotifyingHost (1.0f);
    airParameter->setValueNotifyingHost (1.0f);
    loSlopeParameter->setValueNotifyingHost (1.0f);
    hiSlopeParameter->setValueNotifyingHost (1.0f);
    minPhaseAParameter->setValueNotifyingHost (1.0f);
    minPhaseBParameter->setValueNotifyingHost (1.0f);
    gainModeParameter->setValueNotifyingHost (1.0f);
    alignParameter->setValueNotifyingHost (1.0f);
    delayParameter->setValueNotifyingHost (delayParameter->convertTo0to1 (-3.5f));
    distanceParameter->setValueNotifyingHost (distanceParameter->convertTo0to1 (80.0f));

    for (const auto sampleRate : { 44100.0, 48000.0, 96000.0 })
    {
        for (const auto blockSize : { 32, 128, 512, 1024 })
        {
            processor.prepareToPlay (sampleRate, blockSize);

            CAPTURE (sampleRate, blockSize);
            CHECK (processor.getLatencySamples() == 0);
        }
    }
}
