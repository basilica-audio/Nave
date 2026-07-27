#include "dsp/MorphConvolver.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <random>
#include <vector>

namespace
{
    constexpr double testSampleRate = 48000.0;

    juce::dsp::ProcessSpec makeSpec (int blockSize, int numChannels = 2)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = static_cast<juce::uint32> (numChannels);
        return spec;
    }

    std::vector<float> makeRandomSignal (int numSamples, unsigned int seed)
    {
        std::mt19937 engine (seed);
        std::uniform_real_distribution<float> distribution (-1.0f, 1.0f);

        std::vector<float> signal (static_cast<size_t> (numSamples));

        for (auto& sample : signal)
            sample = distribution (engine);

        return signal;
    }

    // A decaying-noise impulse response, shaped roughly like a real cabinet
    // capture (an immediate transient followed by an exponential decay).
    std::vector<float> makeImpulseResponse (int numSamples, unsigned int seed)
    {
        std::mt19937 engine (seed);
        std::uniform_real_distribution<float> distribution (-1.0f, 1.0f);

        std::vector<float> ir (static_cast<size_t> (numSamples));

        for (int i = 0; i < numSamples; ++i)
        {
            const auto decay = std::exp (-3.0f * static_cast<float> (i) / static_cast<float> (numSamples));
            ir[static_cast<size_t> (i)] = distribution (engine) * decay;
        }

        // A dominant leading tap, as every real IR has.
        ir[0] = 1.0f;

        // Normalised to unit energy, as every IR the engine actually loads is
        // (JUCE's Normalise::yes, or IrLoudness). This keeps the convolved
        // output at roughly input scale for every IR length in the sweep, so a
        // single *absolute* error bound below means the same thing at 1 tap
        // and at 16384 - without it, a long IR's larger output would fail an
        // absolute bound purely for being louder.
        double energy = 0.0;

        for (auto sample : ir)
            energy += static_cast<double> (sample) * sample;

        if (energy > 0.0)
        {
            const auto gain = static_cast<float> (1.0 / std::sqrt (energy));

            for (auto& sample : ir)
                sample *= gain;
        }

        return ir;
    }

    // The reference: plain time-domain FIR convolution, no partitioning, no
    // FFT. Everything the convolver produces is measured against this.
    // Accumulated in double so the reference itself contributes no meaningful
    // error to the comparison - summing 16384 float products in float would
    // otherwise be the least accurate term in the test.
    std::vector<float> directConvolve (const std::vector<float>& input, const std::vector<float>& ir)
    {
        std::vector<float> output (input.size(), 0.0f);

        for (size_t n = 0; n < input.size(); ++n)
        {
            double accumulator = 0.0;

            const auto taps = std::min (ir.size(), n + 1);

            for (size_t k = 0; k < taps; ++k)
                accumulator += static_cast<double> (ir[k]) * input[n - k];

            output[n] = static_cast<float> (accumulator);
        }

        return output;
    }

    // Runs `input` through the convolver in fixed-size blocks and returns the
    // output.
    std::vector<float> runConvolver (MorphConvolver& convolver,
                                      const std::vector<float>& input,
                                      int blockSize,
                                      int numChannels = 1)
    {
        std::vector<float> output;
        output.reserve (input.size());

        juce::AudioBuffer<float> buffer (numChannels, blockSize);

        size_t position = 0;

        while (position < input.size())
        {
            const auto count = static_cast<int> (std::min (static_cast<size_t> (blockSize),
                                                              input.size() - position));

            buffer.clear();

            for (int channel = 0; channel < numChannels; ++channel)
                for (int i = 0; i < count; ++i)
                    buffer.setSample (channel, i, input[position + static_cast<size_t> (i)]);

            juce::dsp::AudioBlock<float> block (buffer);
            auto sub = block.getSubBlock (0, static_cast<size_t> (count));
            convolver.process (sub);

            for (int i = 0; i < count; ++i)
                output.push_back (buffer.getSample (0, i));

            position += static_cast<size_t> (count);
        }

        return output;
    }

    float maxAbsoluteDifference (const std::vector<float>& a, const std::vector<float>& b)
    {
        const auto count = std::min (a.size(), b.size());

        float worst = 0.0f;

        for (size_t i = 0; i < count; ++i)
            worst = juce::jmax (worst, std::abs (a[i] - b[i]));

        return worst;
    }

    // The largest sample-to-sample step in a signal - the click detector used
    // by the IR-exchange tests.
    float maxSampleStep (const std::vector<float>& signal, size_t from, size_t to)
    {
        float worst = 0.0f;

        for (size_t i = std::max<size_t> (1, from); i < std::min (to, signal.size()); ++i)
            worst = juce::jmax (worst, std::abs (signal[i] - signal[i - 1]));

        return worst;
    }
}

//==============================================================================
// Test 1 (merge gate): the convolver must reproduce plain time-domain FIR
// convolution. Every IR length that could exercise a partitioning edge case is
// covered: shorter than a partition, exactly a partition, one either side of
// it, two partitions, a prime length that lands mid-partition, and the maximum
// supported length.
TEST_CASE ("MorphConvolver nulls against direct FIR convolution", "[dsp][morph-convolver]")
{
    constexpr int blockSize = 256;

    const auto irLength = GENERATE (1, 255, 256, 257, 512, 1024, 173, 16384);

    CAPTURE (irLength);

    MorphConvolver convolver;
    convolver.prepare (makeSpec (blockSize, 1));

    const auto ir = makeImpulseResponse (irLength, 1234u + static_cast<unsigned int> (irLength));

    REQUIRE (convolver.publishImpulseResponse (ir.data(), irLength));

    // The first publication goes live without a crossfade, so no settling
    // period is needed before comparing.
    const auto input = makeRandomSignal (juce::jmax (4 * irLength, 4096), 99u);

    const auto actual = runConvolver (convolver, input, blockSize);
    const auto expected = directConvolve (input, ir);

    CHECK (maxAbsoluteDifference (actual, expected) < 1.0e-5f);
}

//==============================================================================
// Test 2 (merge gate): latency is exactly zero. A Kronecker delta in must
// produce the IR's first tap at output index 0, not one partition later.
TEST_CASE ("MorphConvolver reports and realises zero latency", "[dsp][morph-convolver][latency]")
{
    constexpr int blockSize = 128;

    MorphConvolver convolver;
    convolver.prepare (makeSpec (blockSize, 1));

    CHECK (convolver.getLatencySamples() == 0);

    const auto ir = makeImpulseResponse (1024, 7u);
    REQUIRE (convolver.publishImpulseResponse (ir.data(), 1024));

    std::vector<float> impulse (2048, 0.0f);
    impulse[0] = 1.0f;

    const auto output = runConvolver (convolver, impulse, blockSize);

    // The first output sample is the IR's first tap: no leading zeros at all.
    CHECK (output[0] == Catch::Approx (ir[0]).margin (1.0e-5));

    // And the whole response is the IR itself, undelayed.
    for (int i = 0; i < 1024; ++i)
        REQUIRE (output[static_cast<size_t> (i)] == Catch::Approx (ir[static_cast<size_t> (i)]).margin (1.0e-5));
}

//==============================================================================
// Test 3 (merge gate): the output must not depend on how the host happens to
// chop the stream into blocks.
TEST_CASE ("MorphConvolver output is invariant to chunk size", "[dsp][morph-convolver]")
{
    constexpr int maxBlockSize = 512;
    constexpr int irLength = 2048;

    const auto ir = makeImpulseResponse (irLength, 42u);
    const auto input = makeRandomSignal (16384, 4242u);

    MorphConvolver reference;
    reference.prepare (makeSpec (maxBlockSize, 1));
    REQUIRE (reference.publishImpulseResponse (ir.data(), irLength));

    const auto referenceOutput = runConvolver (reference, input, maxBlockSize);

    // Randomised, seeded chunk sizes between 1 and maxBlockSize.
    MorphConvolver fuzzed;
    fuzzed.prepare (makeSpec (maxBlockSize, 1));
    REQUIRE (fuzzed.publishImpulseResponse (ir.data(), irLength));

    std::mt19937 engine (2026u);
    std::uniform_int_distribution<int> chunkDistribution (1, maxBlockSize);

    juce::AudioBuffer<float> buffer (1, maxBlockSize);

    std::vector<float> fuzzedOutput;
    fuzzedOutput.reserve (input.size());

    size_t position = 0;

    while (position < input.size())
    {
        const auto count = static_cast<int> (std::min (static_cast<size_t> (chunkDistribution (engine)),
                                                          input.size() - position));

        buffer.clear();

        for (int i = 0; i < count; ++i)
            buffer.setSample (0, i, input[position + static_cast<size_t> (i)]);

        juce::dsp::AudioBlock<float> block (buffer);
        auto sub = block.getSubBlock (0, static_cast<size_t> (count));
        fuzzed.process (sub);

        for (int i = 0; i < count; ++i)
            fuzzedOutput.push_back (buffer.getSample (0, i));

        position += static_cast<size_t> (count);
    }

    // Mathematically these two runs are identical: chunking changes only how
    // much of a partition is filled when each FFT is taken, never what the
    // convolution sum evaluates to. The residual is therefore pure
    // single-precision rounding - the zero-latency scheme re-transforms a
    // differently zero-padded window in each run, so the two FFT chains round
    // differently even though they compute the same quantity. Exact equality
    // is unreachable in float by construction; measured worst case here is
    // ~4.2e-7 against an output peaking around 2.5, i.e. ~1.7e-7 relative,
    // which is the accumulation floor for a 1024-point float FFT round trip.
    // A structural chunk dependency (a real bug) would show up orders of
    // magnitude above this bound, not just under it.
    CHECK (maxAbsoluteDifference (referenceOutput, fuzzedOutput) < 1.0e-6f);
}

//==============================================================================
// Test 4 (merge gate): the IR exchange must be click-free, and republishing an
// IDENTICAL IR must be inaudible. This is the test the equal-power crossfade
// the research inputs prescribed would fail: sin/cos gains sum to sqrt(2) for
// perfectly correlated signals, so an identical-IR republish would peak +3 dB
// at the fade midpoint instead of nulling.
TEST_CASE ("MorphConvolver exchanges impulse responses without artefacts", "[dsp][morph-convolver]")
{
    constexpr int blockSize = 128;
    constexpr int irLength = 1024;
    constexpr int totalSamples = 24000;

    const auto irA = makeImpulseResponse (irLength, 11u);
    const auto input = makeRandomSignal (totalSamples, 777u);

    // Reference run: no swap at all.
    MorphConvolver noSwap;
    noSwap.prepare (makeSpec (blockSize, 1));
    REQUIRE (noSwap.publishImpulseResponse (irA.data(), irLength));
    const auto reference = runConvolver (noSwap, input, blockSize);

    SECTION ("republishing an identical IR nulls against the no-swap run")
    {
        MorphConvolver convolver;
        convolver.prepare (makeSpec (blockSize, 1));
        REQUIRE (convolver.publishImpulseResponse (irA.data(), irLength));

        juce::AudioBuffer<float> buffer (1, blockSize);

        std::vector<float> output;
        output.reserve (input.size());

        size_t position = 0;
        bool republished = false;

        while (position < input.size())
        {
            const auto count = static_cast<int> (std::min (static_cast<size_t> (blockSize),
                                                              input.size() - position));

            // Republish the very same IR halfway through the signal.
            if (! republished && position > input.size() / 2)
            {
                REQUIRE (convolver.publishImpulseResponse (irA.data(), irLength));
                republished = true;
            }

            buffer.clear();

            for (int i = 0; i < count; ++i)
                buffer.setSample (0, i, input[position + static_cast<size_t> (i)]);

            juce::dsp::AudioBlock<float> block (buffer);
            auto sub = block.getSubBlock (0, static_cast<size_t> (count));
            convolver.process (sub);

            for (int i = 0; i < count; ++i)
                output.push_back (buffer.getSample (0, i));

            position += static_cast<size_t> (count);
        }

        REQUIRE (republished);

        const auto residual = maxAbsoluteDifference (reference, output);
        const auto residualDb = juce::Decibels::gainToDecibels (residual, -200.0f);

        CAPTURE (residualDb);
        CHECK (residualDb < -100.0f);
    }

    SECTION ("swapping to a different IR produces no click")
    {
        const auto irB = makeImpulseResponse (irLength, 22u);

        MorphConvolver convolver;
        convolver.prepare (makeSpec (blockSize, 1));
        REQUIRE (convolver.publishImpulseResponse (irA.data(), irLength));

        juce::AudioBuffer<float> buffer (1, blockSize);

        std::vector<float> output;
        output.reserve (input.size());

        size_t position = 0;
        size_t swapPosition = 0;

        while (position < input.size())
        {
            const auto count = static_cast<int> (std::min (static_cast<size_t> (blockSize),
                                                              input.size() - position));

            if (swapPosition == 0 && position > input.size() / 2)
            {
                REQUIRE (convolver.publishImpulseResponse (irB.data(), irLength));
                swapPosition = position;
            }

            buffer.clear();

            for (int i = 0; i < count; ++i)
                buffer.setSample (0, i, input[position + static_cast<size_t> (i)]);

            juce::dsp::AudioBlock<float> block (buffer);
            auto sub = block.getSubBlock (0, static_cast<size_t> (count));
            convolver.process (sub);

            for (int i = 0; i < count; ++i)
                output.push_back (buffer.getSample (0, i));

            position += static_cast<size_t> (count);
        }

        REQUIRE (swapPosition > 0);

        // Steady state measured well before the swap, the fade window
        // generously around it.
        const auto steadyStateStep = maxSampleStep (output, 1000, swapPosition - 500);
        const auto fadeStep = maxSampleStep (output, swapPosition, swapPosition + 4000);

        CAPTURE (steadyStateStep);
        CAPTURE (fadeStep);

        CHECK (fadeStep < 3.0f * steadyStateStep);
    }
}

//==============================================================================
// Stereo sanity: both channels must convolve independently and identically
// when fed identical input.
TEST_CASE ("MorphConvolver processes stereo channels independently", "[dsp][morph-convolver]")
{
    constexpr int blockSize = 256;
    constexpr int irLength = 512;

    MorphConvolver convolver;
    convolver.prepare (makeSpec (blockSize, 2));

    const auto ir = makeImpulseResponse (irLength, 5u);
    REQUIRE (convolver.publishImpulseResponse (ir.data(), irLength));

    juce::AudioBuffer<float> buffer (2, blockSize);
    TestHelpers::fillWithSine (buffer, testSampleRate, 440.0);

    // Make the right channel a scaled copy so the check below is meaningful.
    buffer.applyGain (1, 0, blockSize, 0.5f);

    juce::AudioBuffer<float> expectedLeft (1, blockSize);
    expectedLeft.copyFrom (0, 0, buffer, 0, 0, blockSize);

    juce::dsp::AudioBlock<float> block (buffer);
    convolver.process (block);

    for (int i = 0; i < blockSize; ++i)
        REQUIRE (buffer.getSample (1, i) == Catch::Approx (buffer.getSample (0, i) * 0.5f).margin (1.0e-5));

    CHECK (TestHelpers::allSamplesFinite (buffer));
}
