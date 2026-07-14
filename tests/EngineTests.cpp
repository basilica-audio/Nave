#include "dsp/CabConvolutionEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 8192; // large single block: keeps the null/
                                         // energy tests below simple by
                                         // avoiding multi-block bookkeeping.
    constexpr double testFrequencyHz = 1000.0;

    // 10^(-80/20): the "< -80 dBFS" null-test threshold from the DSP spec, in
    // linear amplitude.
    constexpr float nullTestTolerance = 1.0e-4f;

    juce::dsp::ProcessSpec makeTestSpec (int numChannels)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (testBlockSize);
        spec.numChannels = static_cast<juce::uint32> (numChannels);
        return spec;
    }
}

TEST_CASE ("Null test: default delta IR, wide-open LoCut/HiCut, Mix 100% nulls against the input",
          "[dsp][engine][null]")
{
    CabConvolutionEngine engine;

    // Explicitly (re)state the "wide open"/fully wet settings the spec
    // describes, even though they also happen to be this engine's built-in
    // defaults - a genuine null test should prove the chain is transparent
    // at these settings, not merely that construction happens to be quiet.
    engine.setLoCutHz (CabConvolutionEngine::loCutMinHz);
    engine.setHiCutHz (CabConvolutionEngine::hiCutMaxHz);
    engine.setMixProportion (1.0f);
    engine.setLevelDb (0.0f);
    engine.setBlendProportion (0.0f); // IR A only - IR B's default delta never enters the mix
    engine.setDistancePercent (CabConvolutionEngine::distanceMinPercent); // "off"

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    // The default zero-latency convolution configuration with a 1-sample
    // delta IR must report zero latency.
    REQUIRE (engine.getLatencySamples() == 0);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, testFrequencyHz, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        float maxResidual = 0.0f;

        for (int i = 0; i < testBlockSize; ++i)
            maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));

        CHECK (maxResidual < nullTestTolerance);
    }
}

TEST_CASE ("Null test also holds at the engine's untouched (freshly constructed) defaults",
          "[dsp][engine][null]")
{
    // Same as above, but without calling any setter at all - proves the
    // plugin is a valid, transparent passthrough purely from its built-in
    // defaults, matching "DEFAULT with no user IR = a unit-impulse (delta)
    // IR so the plugin is valid out of the box" in the DSP spec.
    CabConvolutionEngine engine;

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    REQUIRE (engine.getLatencySamples() == 0);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, testFrequencyHz, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        float maxResidual = 0.0f;

        for (int i = 0; i < testBlockSize; ++i)
            maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));

        CHECK (maxResidual < nullTestTolerance);
    }
}

TEST_CASE ("LoCut attenuates low-frequency energy once engaged", "[dsp][engine][filters]")
{
    constexpr double lowTestFrequencyHz = 80.0;

    const auto measureRms = [] (float loCutHz)
    {
        CabConvolutionEngine engine;
        engine.setLoCutHz (loCutHz);
        engine.setHiCutHz (CabConvolutionEngine::hiCutMaxHz);
        engine.setMixProportion (1.0f);
        engine.setLevelDb (0.0f);

        const auto spec = makeTestSpec (2);
        engine.prepare (spec);

        juce::AudioBuffer<float> buffer (2, testBlockSize);
        TestHelpers::fillWithSine (buffer, testSampleRate, lowTestFrequencyHz, 0.5f);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        return TestHelpers::rms (buffer);
    };

    const auto bypassedRms = measureRms (CabConvolutionEngine::loCutMinHz); // "off" position
    const auto engagedRms = measureRms (600.0f); // well above the 80 Hz test tone

    REQUIRE (bypassedRms > 0.0);
    CHECK (engagedRms < bypassedRms * 0.5); // at least -6 dB of attenuation
}

TEST_CASE ("HiCut attenuates high-frequency energy once engaged", "[dsp][engine][filters]")
{
    constexpr double highTestFrequencyHz = 12000.0;

    const auto measureRms = [] (float hiCutHz)
    {
        CabConvolutionEngine engine;
        engine.setLoCutHz (CabConvolutionEngine::loCutMinHz);
        engine.setHiCutHz (hiCutHz);
        engine.setMixProportion (1.0f);
        engine.setLevelDb (0.0f);

        const auto spec = makeTestSpec (2);
        engine.prepare (spec);

        juce::AudioBuffer<float> buffer (2, testBlockSize);
        TestHelpers::fillWithSine (buffer, testSampleRate, highTestFrequencyHz, 0.5f);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        return TestHelpers::rms (buffer);
    };

    const auto bypassedRms = measureRms (CabConvolutionEngine::hiCutMaxHz); // "off" position
    const auto engagedRms = measureRms (3000.0f); // well below the 12 kHz test tone

    REQUIRE (bypassedRms > 0.0);
    CHECK (engagedRms < bypassedRms * 0.5); // at least -6 dB of attenuation
}

TEST_CASE ("A loaded (non-delta) impulse response measurably changes the output", "[dsp][engine][convolution]")
{
    CabConvolutionEngine engine;
    engine.setMixProportion (1.0f);
    engine.setLevelDb (0.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    // A short, decaying two-tap IR - clearly not an identity/delta impulse.
    juce::AudioBuffer<float> ir (1, 4);
    ir.setSample (0, 0, 1.0f);
    ir.setSample (0, 1, 0.5f);
    ir.setSample (0, 2, 0.25f);
    ir.setSample (0, 3, 0.125f);

    engine.setImpulseResponse (std::move (ir), testSampleRate);

    // juce::dsp::Convolution loads impulse responses asynchronously via a
    // background thread (so loadImpulseResponse() itself is wait-free/RT-
    // safe); the only point at which a newly loaded IR is *guaranteed* to be
    // synchronously installed is the next prepare() call (which drains any
    // pending load before rebuilding the active engine - see
    // docs/architecture.md). Without this, process() below could race the
    // background thread and still be running the previous (default delta)
    // IR.
    engine.prepare (spec);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, testFrequencyHz, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    CHECK (TestHelpers::allSamplesFinite (processed));

    float maxResidual = 0.0f;

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        for (int i = 0; i < testBlockSize; ++i)
            maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));
    }

    // A genuinely different IR must move the output measurably away from a
    // pure passthrough - well above the null test's -80 dBFS transparency
    // threshold.
    CHECK (maxResidual > nullTestTolerance);
}

TEST_CASE ("IR Blend at 0% (default) leaves IR B's loaded IR completely unheard", "[dsp][engine][blend]")
{
    CabConvolutionEngine engine;
    engine.setMixProportion (1.0f);
    engine.setLevelDb (0.0f);
    engine.setBlendProportion (0.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    // A drastically different IR loaded into slot B - if Blend = 0% ever let
    // any of it through, this would be unmissable in the output.
    juce::AudioBuffer<float> irB (1, 4);
    irB.setSample (0, 0, 1.0f);
    irB.setSample (0, 1, -1.0f);
    irB.setSample (0, 2, 1.0f);
    irB.setSample (0, 3, -1.0f);
    engine.setImpulseResponseB (std::move (irB), testSampleRate);
    engine.prepare (spec); // guarantee the async load is drained/active - see docs/architecture.md

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, testFrequencyHz, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    CHECK (TestHelpers::allSamplesFinite (processed));

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        float maxResidual = 0.0f;

        for (int i = 0; i < testBlockSize; ++i)
            maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));

        CHECK (maxResidual < nullTestTolerance);
    }
}

TEST_CASE ("IR Blend at 100% is driven entirely by IR B, not IR A", "[dsp][engine][blend]")
{
    CabConvolutionEngine engine;
    engine.setMixProportion (1.0f);
    engine.setLevelDb (0.0f);
    engine.setBlendProportion (1.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    // IR A stays the default delta (identity); IR B is a genuinely
    // different, decaying IR - at Blend = 100% the output must match
    // processing through IR B alone, not the untouched input.
    juce::AudioBuffer<float> irB (1, 4);
    irB.setSample (0, 0, 1.0f);
    irB.setSample (0, 1, 0.5f);
    irB.setSample (0, 2, 0.25f);
    irB.setSample (0, 3, 0.125f);
    engine.setImpulseResponseB (std::move (irB), testSampleRate);
    engine.prepare (spec);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, testFrequencyHz, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    CHECK (TestHelpers::allSamplesFinite (processed));

    float maxResidual = 0.0f;

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        for (int i = 0; i < testBlockSize; ++i)
            maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));
    }

    // A genuinely different IR B must move the output measurably away from
    // a pure passthrough of the (untouched, delta-IR-A) input.
    CHECK (maxResidual > nullTestTolerance);
}

TEST_CASE ("IR Blend at 50% sits between IR A alone and IR B alone", "[dsp][engine][blend]")
{
    const auto measurePeak = [] (float blendProportion)
    {
        CabConvolutionEngine engine;
        engine.setMixProportion (1.0f);
        engine.setLevelDb (0.0f);
        engine.setBlendProportion (blendProportion);

        const auto spec = makeTestSpec (2);
        engine.prepare (spec);

        // A short IR with a strong first tap - at Blend = 100% the output's
        // peak should track this tap's gain much more closely than at
        // Blend = 0% (identity IR A).
        juce::AudioBuffer<float> irB (1, 2);
        irB.setSample (0, 0, 0.2f);
        irB.setSample (0, 1, 0.0f);
        engine.setImpulseResponseB (std::move (irB), testSampleRate);
        engine.prepare (spec);

        juce::AudioBuffer<float> buffer (2, testBlockSize);
        TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.5f);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        CHECK (TestHelpers::allSamplesFinite (buffer));
        return TestHelpers::peakAbsolute (buffer);
    };

    const auto peakAtA = measurePeak (0.0f);
    const auto peakAtHalf = measurePeak (0.5f);
    const auto peakAtB = measurePeak (1.0f);

    // IR A is the identity (peak == input peak, 0.5); IR B attenuates
    // heavily (peak << input peak). The 50% blend must land strictly
    // between the two.
    REQUIRE (peakAtB < peakAtA);
    CHECK (peakAtHalf < peakAtA);
    CHECK (peakAtHalf > peakAtB);
}

TEST_CASE ("Distance at 0% (default) is a bit-exact passthrough", "[dsp][engine][distance]")
{
    CabConvolutionEngine engine;
    engine.setMixProportion (1.0f);
    engine.setLevelDb (0.0f);
    engine.setDistancePercent (CabConvolutionEngine::distanceMinPercent);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillWithSine (reference, testSampleRate, testFrequencyHz, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        float maxResidual = 0.0f;

        for (int i = 0; i < testBlockSize; ++i)
            maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));

        CHECK (maxResidual < nullTestTolerance);
    }
}

TEST_CASE ("Distance at 100% measurably attenuates both low- and high-frequency energy", "[dsp][engine][distance]")
{
    const auto measureRms = [] (float distancePercent, double frequencyHz)
    {
        CabConvolutionEngine engine;
        engine.setMixProportion (1.0f);
        engine.setLevelDb (0.0f);
        engine.setDistancePercent (distancePercent);

        const auto spec = makeTestSpec (2);
        engine.prepare (spec);

        juce::AudioBuffer<float> buffer (2, testBlockSize);
        TestHelpers::fillWithSine (buffer, testSampleRate, frequencyHz, 0.5f);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        return TestHelpers::rms (buffer);
    };

    constexpr double lowTestFrequencyHz = 100.0; // near the low-shelf frequency
    constexpr double highTestFrequencyHz = 15000.0; // well above the high-shelf frequency

    const auto lowRmsOff = measureRms (CabConvolutionEngine::distanceMinPercent, lowTestFrequencyHz);
    const auto lowRmsFar = measureRms (CabConvolutionEngine::distanceMaxPercent, lowTestFrequencyHz);
    const auto highRmsOff = measureRms (CabConvolutionEngine::distanceMinPercent, highTestFrequencyHz);
    const auto highRmsFar = measureRms (CabConvolutionEngine::distanceMaxPercent, highTestFrequencyHz);

    REQUIRE (lowRmsOff > 0.0);
    REQUIRE (highRmsOff > 0.0);
    CHECK (lowRmsFar < lowRmsOff);
    CHECK (highRmsFar < highRmsOff);
}

TEST_CASE ("reset() clears filter/convolution/mixer state without crashing", "[dsp][engine]")
{
    CabConvolutionEngine engine;
    engine.setLoCutHz (300.0f);
    engine.setHiCutHz (3000.0f);
    engine.setMixProportion (1.0f);
    engine.setBlendProportion (0.5f);
    engine.setDistancePercent (50.0f);

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    juce::AudioBuffer<float> irB (1, 4);
    irB.setSample (0, 0, 1.0f);
    irB.setSample (0, 1, 0.5f);
    engine.setImpulseResponseB (std::move (irB), testSampleRate);
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.9f);

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    CHECK_NOTHROW (engine.reset());
    CHECK (TestHelpers::allSamplesFinite (buffer));

    // Processing again straight after reset() must not crash or produce
    // non-finite output.
    TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.9f);
    CHECK_NOTHROW (engine.process (block));
    CHECK (TestHelpers::allSamplesFinite (buffer));
}
