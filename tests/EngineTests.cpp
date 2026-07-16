#include "dsp/CabConvolutionEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <functional>

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

// Normalisation-awareness note (design-brief.md SS5, research-notes.md SS5):
// juce::dsp::Convolution's Normalise::yes rescales each loaded IR to a fixed
// *energy* target (JUCE forum: normalizationFactor = 0.125f /
// sqrt(sumOfSquaredMagnitudes)), not a perceptual loudness match - two
// real-world cab IRs of different length/spectral density can land at
// audibly different output levels after loading even though both were
// "normalised". There is no way to unit-test perceptual loudness-matching
// without reference audio, so this is deliberately just a comment, not a
// new TEST_CASE: docs/manual.md's "Loading impulse responses" section now
// carries the user-facing version of this same callout (pointing at Level
// as the fix), so a future contributor investigating a "why do these two
// IRs sound different loudness" bug report finds the documented explanation
// here and in the manual instead of re-discovering it from scratch.
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

TEST_CASE ("IR Blend with two distinct, non-identity IRs in both slots blends them in parallel, not in series",
           "[dsp][engine][blend]")
{
    // Regression coverage for the plugin's headline IR Blend use case: two
    // independently-captured, non-identity cab IRs (e.g. "a tight 4x12 with
    // a boomier 2x12"), unlike every other blend test above, which leaves
    // IR A at the default identity/delta IR and so cannot distinguish a
    // correct parallel blend from an erroneous cascaded one (IR B applied
    // on top of IR A's already-convolved output instead of the original dry
    // input) - both slots below carry real, decaying, non-delta taps.
    const auto makeIrA = []
    {
        juce::AudioBuffer<float> ir (1, 4);
        ir.setSample (0, 0, 1.0f);
        ir.setSample (0, 1, 0.6f);
        ir.setSample (0, 2, 0.3f);
        ir.setSample (0, 3, 0.1f);
        return ir;
    };

    const auto makeIrB = []
    {
        juce::AudioBuffer<float> ir (1, 3);
        ir.setSample (0, 0, 0.8f);
        ir.setSample (0, 1, -0.4f);
        ir.setSample (0, 2, 0.2f);
        return ir;
    };

    // Ground truth for IR_A(input) and IR_B(input): each rendered by a
    // *separate* engine with the candidate IR loaded into slot A only and
    // Blend left at 0%, so process() never enters the blendEngaged branch at
    // all. This is deliberate and load-bearing - if these references were
    // instead captured via the dual-slot engine at Blend = 0%/100% (as the
    // other blend tests above do), a cascaded implementation (IR B applied
    // to IR A's already-convolved output) would still be perfectly linear in
    // Blend once engaged, so an intermediate blend would match a linear
    // interpolation of *those* (equally cascaded) references - silently
    // passing despite the defect. Only comparing against references that
    // never touch the blend branch actually exercises whether IR B receives
    // the original dry input or IR A's output.
    const auto renderIrAlone = [&] (const std::function<juce::AudioBuffer<float>()>& makeIr)
    {
        CabConvolutionEngine engine;
        engine.setMixProportion (1.0f);
        engine.setLevelDb (0.0f);
        engine.setBlendProportion (0.0f);

        const auto spec = makeTestSpec (2);
        engine.prepare (spec);

        engine.setImpulseResponse (makeIr(), testSampleRate);
        engine.prepare (spec); // guarantee the async load is drained/active

        juce::AudioBuffer<float> buffer (2, testBlockSize);
        TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.5f);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        CHECK (TestHelpers::allSamplesFinite (buffer));
        return buffer;
    };

    const auto renderBlended = [&] (float blendProportion)
    {
        CabConvolutionEngine engine;
        engine.setMixProportion (1.0f);
        engine.setLevelDb (0.0f);
        engine.setBlendProportion (blendProportion);

        const auto spec = makeTestSpec (2);
        engine.prepare (spec);

        engine.setImpulseResponse (makeIrA(), testSampleRate);
        engine.setImpulseResponseB (makeIrB(), testSampleRate);
        engine.prepare (spec); // guarantee both async loads are drained/active

        juce::AudioBuffer<float> buffer (2, testBlockSize);
        TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.5f);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        CHECK (TestHelpers::allSamplesFinite (buffer));
        return buffer;
    };

    const auto pureA = renderIrAlone (makeIrA);
    const auto pureB = renderIrAlone (makeIrB);
    const auto blendedQuarter = renderBlended (0.25f);

    // Sanity: the two references must actually differ, otherwise the check
    // below would be vacuous.
    REQUIRE (std::abs (TestHelpers::rms (pureA) - TestHelpers::rms (pureB)) > 1.0e-3);

    constexpr float parallelBlendTolerance = 1.0e-4f;

    for (int channel = 0; channel < blendedQuarter.getNumChannels(); ++channel)
    {
        const auto* aData = pureA.getReadPointer (channel);
        const auto* bData = pureB.getReadPointer (channel);
        const auto* mixedData = blendedQuarter.getReadPointer (channel);

        float maxResidual = 0.0f;

        for (int i = 0; i < testBlockSize; ++i)
        {
            // The correct, parallel crossfade: (1 - blend) * IR_A(input) +
            // blend * IR_B(input). A cascaded implementation (IR B applied
            // to IR A's output) would diverge from this by far more than
            // floating-point noise, since both IRs here are genuinely
            // different, non-identity filters.
            const auto expected = 0.75f * aData[i] + 0.25f * bData[i];
            maxResidual = std::max (maxResidual, std::abs (mixedData[i] - expected));
        }

        CHECK (maxResidual < parallelBlendTolerance);
    }
}

TEST_CASE ("convolutionB is reset on Blend's disengaged->engaged transition, not left carrying a stale overlap tail",
           "[dsp][engine][blend]")
{
    // Regression coverage for #12. juce::dsp::Convolution's zero-latency
    // uniformly-partitioned algorithm carries an internal overlap-add tail
    // ("bufferOverlap"/"overlapData" in JUCE 8.0.14's juce_Convolution.cpp)
    // that is only ever mutated inside processSamples() - process() below
    // skips convolutionB.process() entirely for every block Blend is
    // disengaged, so that tail is frozen (not decaying) for as long as
    // Blend stays disengaged.
    //
    // Each block here is a single full-size (testBlockSize) call rather than
    // many small ones deliberately: testBlockSize's duration (~170 ms at
    // 48 kHz) comfortably exceeds the engine's internal parameter-smoothing
    // ramp (50 ms, see CabConvolutionEngine::smoothingTimeSeconds), so
    // blendSmoothed.skip() settles fully to its new target *within* the very
    // next block - Blend's disengaged/engaged state is then clean and
    // block-atomic, with no partially-ramped blocks in between to muddy
    // which state was actually in effect when.
    constexpr int irLength = 512; // several times shorter than testBlockSize,
                                   // so its full ring-down tail fits inside
                                   // one process() call's overlap-add state

    CabConvolutionEngine engine;
    engine.setMixProportion (1.0f);
    engine.setLevelDb (0.0f);
    engine.setBlendProportion (1.0f); // engaged from the start

    const auto spec = makeTestSpec (2);
    engine.prepare (spec);

    // A long, high-energy, slowly-decaying IR B, so any leaked overlap tail
    // is unmistakable rather than lost in floating-point noise.
    juce::AudioBuffer<float> irB (1, irLength);
    for (int i = 0; i < irLength; ++i)
        irB.setSample (0, i, 0.8f * std::pow (0.995f, static_cast<float> (i)));

    engine.setImpulseResponseB (std::move (irB), testSampleRate);
    engine.prepare (spec); // guarantee the async load is drained/active

    juce::AudioBuffer<float> buffer (2, testBlockSize);

    // One loud, engaged block: builds real, non-trivial state in
    // convolutionB's internal overlap-add buffer (its ring-down tail
    // extends past this block's end, into what would be the next call).
    TestHelpers::fillWithSine (buffer, testSampleRate, testFrequencyHz, 0.9f);
    juce::dsp::AudioBlock<float> engagedBlock (buffer);
    engine.process (engagedBlock);
    CHECK (TestHelpers::allSamplesFinite (buffer));

    // Disengage Blend for one full block. convolutionB.process() is skipped
    // entirely for it, so its internal state - including that ring-down
    // tail - is frozen exactly where the block above left it, not decayed.
    engine.setBlendProportion (0.0f);
    buffer.clear();
    juce::dsp::AudioBlock<float> disengagedBlock (buffer);
    engine.process (disengagedBlock);

    // Re-engage Blend and feed pure silence. With convolutionB correctly
    // reset on the disengaged->engaged transition, silence in must produce
    // silence out. Without the reset (the bug), the frozen overlap-add tail
    // from the loud block above gets added back into this block's output.
    engine.setBlendProportion (1.0f);
    buffer.clear();

    juce::dsp::AudioBlock<float> reengagedBlock (buffer);
    engine.process (reengagedBlock);

    CHECK (TestHelpers::allSamplesFinite (buffer));
    CHECK (TestHelpers::peakAbsolute (buffer) < nullTestTolerance);
}

TEST_CASE ("Reloading IR A after IR B re-aligns IR B against the new reference, not the stale one",
           "[dsp][engine][blend][ir-alignment]")
{
    // Regression coverage for #13. setImpulseResponseB() aligns IR B
    // against whatever IR A onset is current *at the moment it's called*;
    // reloading IR A afterwards must re-run that alignment against the new
    // reference, or IR B silently stays aligned to the stale, overwritten
    // onset. Compares an engine that reloads IR A after IR B (the buggy
    // sequence) against a reference engine that loads IR A with the same
    // final onset *before* IR B is ever loaded (so IR B's alignment there
    // is, by construction, never stale) - a correct implementation must
    // make both produce the same IR-B-driven output.
    constexpr int totalSamples = 96;

    const auto makeTransientAt = [] (int onsetSample)
    {
        juce::AudioBuffer<float> buffer (1, totalSamples);
        buffer.clear();

        for (int i = onsetSample; i < totalSamples; ++i)
            buffer.setSample (0, i, std::pow (0.7f, static_cast<float> (i - onsetSample)));

        return buffer;
    };

    const auto renderIrBOnly = [&] (const std::function<void (CabConvolutionEngine&, const juce::dsp::ProcessSpec&)>& loadSequence)
    {
        CabConvolutionEngine engine;
        engine.setMixProportion (1.0f);
        engine.setLevelDb (0.0f);
        engine.setBlendProportion (1.0f); // IR B only, so IR A's final content is irrelevant to the output

        const auto spec = makeTestSpec (2);
        engine.prepare (spec);

        loadSequence (engine, spec);

        juce::AudioBuffer<float> impulse (2, totalSamples);
        impulse.clear();
        impulse.setSample (0, 0, 1.0f);
        impulse.setSample (1, 0, 1.0f);

        juce::dsp::AudioBlock<float> block (impulse);
        engine.process (block);

        REQUIRE (TestHelpers::allSamplesFinite (impulse));
        return impulse;
    };

    // Reference: IR A goes straight to its final onset (10) before IR B is
    // ever loaded - IR B's alignment here can never be stale.
    const auto expected = renderIrBOnly ([&] (CabConvolutionEngine& engine, const juce::dsp::ProcessSpec& spec)
    {
        engine.setImpulseResponse (makeTransientAt (10), testSampleRate);
        engine.prepare (spec);
        engine.setImpulseResponseB (makeTransientAt (30), testSampleRate);
        engine.prepare (spec);
    });

    // Actual: IR A first loaded with onset 0 (so IR B aligns against that),
    // then reloaded with onset 10 - the sequence #13 describes.
    const auto actual = renderIrBOnly ([&] (CabConvolutionEngine& engine, const juce::dsp::ProcessSpec& spec)
    {
        engine.setImpulseResponse (makeTransientAt (0), testSampleRate);
        engine.prepare (spec);
        engine.setImpulseResponseB (makeTransientAt (30), testSampleRate);
        engine.prepare (spec);
        engine.setImpulseResponse (makeTransientAt (10), testSampleRate);
        engine.prepare (spec);
    });

    for (int channel = 0; channel < expected.getNumChannels(); ++channel)
    {
        const auto* expectedData = expected.getReadPointer (channel);
        const auto* actualData = actual.getReadPointer (channel);

        float maxResidual = 0.0f;

        for (int i = 0; i < totalSamples; ++i)
            maxResidual = std::max (maxResidual, std::abs (actualData[i] - expectedData[i]));

        CHECK (maxResidual < nullTestTolerance);
    }
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

//==============================================================================
// v0.2.0 (design-brief.md, "Distance" module spec): the low-shelf's taper
// changed from plain-linear-in-dB to a front-loaded curve. These three
// TEST_CASEs are the brief's own "Catch2 test guarantees" section, verbatim.

TEST_CASE ("Distance's low-shelf taper is front-loaded: more cut in the first half of the sweep than the second",
           "[dsp][engine][distance][taper]")
{
    // 40 Hz sits comfortably below the 200 Hz low-shelf corner, so the
    // measured attenuation closely tracks the shelf's f->0 plateau gain -
    // the same value CabConvolutionEngine.cpp's tapered() helper targets.
    constexpr double lowShelfProbeFrequencyHz = 40.0;
    constexpr float inputAmplitude = 0.5f;

    const auto measureCutDb = [] (float distancePercent)
    {
        CabConvolutionEngine engine;
        engine.setMixProportion (1.0f);
        engine.setLevelDb (0.0f);
        engine.setDistancePercent (distancePercent);

        const auto spec = makeTestSpec (2);
        engine.prepare (spec);

        juce::AudioBuffer<float> buffer (2, testBlockSize);
        TestHelpers::fillWithSine (buffer, testSampleRate, lowShelfProbeFrequencyHz, inputAmplitude);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        CHECK (TestHelpers::allSamplesFinite (buffer));

        const auto outRms = TestHelpers::rms (buffer);
        const auto inRms = static_cast<double> (inputAmplitude) / std::sqrt (2.0);

        return juce::Decibels::gainToDecibels (outRms / inRms);
    };

    const auto cutAt0 = measureCutDb (0.0f);
    const auto cutAt50 = measureCutDb (50.0f);
    const auto cutAt100 = measureCutDb (100.0f);

    // Monotonically non-increasing: more Distance never means less cut.
    REQUIRE (cutAt50 <= cutAt0 + 1.0e-2);
    REQUIRE (cutAt100 <= cutAt50 + 1.0e-2);

    // The brief's own measurable proxy for "front-loaded": the first half of
    // the sweep (0 -> 50%) must cover *more* of the total cut range than the
    // second half (50% -> 100%).
    const auto firstHalfCutDb = cutAt0 - cutAt50;
    const auto secondHalfCutDb = cutAt50 - cutAt100;

    CHECK (firstHalfCutDb > secondHalfCutDb);
}

TEST_CASE ("Distance's low-shelf taper regression guard: fixed-point snapshot at 25/50/75/100%",
           "[dsp][engine][distance][taper]")
{
    // Guards against a future accidental taper-curve regression (same
    // pattern as the existing bypass-epsilon snapshot checks elsewhere in
    // this file). The expected figures are the analytic f->0 plateau values
    // of CabConvolutionEngine.cpp's tapered() ease-out curve
    // (1 - (1 - normalisedDistance)^1.8, scaled by the -6 dB max cut) - not
    // a match to any sourced hardware curve (design-brief.md's Honesty
    // section: no such curve was sourced for the exact taper shape). The
    // tolerance is wide enough to absorb the small, expected gap between a
    // finite test frequency's measured attenuation and that analytic
    // plateau.
    constexpr double lowShelfProbeFrequencyHz = 40.0;
    constexpr int snapshotBlockSize = 65536; // long enough for a stable RMS reading at 40 Hz
    constexpr float inputAmplitude = 0.5f;
    constexpr float toleranceDb = 1.0f;

    const auto measureCutDb = [] (float distancePercent)
    {
        CabConvolutionEngine engine;
        engine.setMixProportion (1.0f);
        engine.setLevelDb (0.0f);
        engine.setDistancePercent (distancePercent);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (snapshotBlockSize);
        spec.numChannels = 2;
        engine.prepare (spec);

        juce::AudioBuffer<float> buffer (2, snapshotBlockSize);
        TestHelpers::fillWithSine (buffer, testSampleRate, lowShelfProbeFrequencyHz, inputAmplitude);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        CHECK (TestHelpers::allSamplesFinite (buffer));

        const auto outRms = TestHelpers::rms (buffer);
        const auto inRms = static_cast<double> (inputAmplitude) / std::sqrt (2.0);

        return juce::Decibels::gainToDecibels (outRms / inRms);
    };

    CHECK (measureCutDb (25.0f) == Catch::Approx (-2.42).margin (toleranceDb));
    CHECK (measureCutDb (50.0f) == Catch::Approx (-4.28).margin (toleranceDb));
    CHECK (measureCutDb (75.0f) == Catch::Approx (-5.51).margin (toleranceDb));
    CHECK (measureCutDb (100.0f) == Catch::Approx (-6.00).margin (toleranceDb));
}

TEST_CASE ("LoCut/HiCut ranges match the v2 brief's deliberate-keep decision", "[dsp][engine][ranges]")
{
    // design-brief.md's LoCut/HiCut module specs: both ranges were
    // considered against the NadIR reference (10-400 Hz / 6-22 kHz) and
    // *deliberately* kept wider than that reference rather than narrowed -
    // cheap insurance against that documented decision silently drifting
    // the next time someone touches these constants.
    CHECK (CabConvolutionEngine::loCutMinHz == 20.0f);
    CHECK (CabConvolutionEngine::loCutMaxHz == 800.0f);
    CHECK (CabConvolutionEngine::hiCutMinHz == 2000.0f);
    CHECK (CabConvolutionEngine::hiCutMaxHz == 20000.0f);
}
