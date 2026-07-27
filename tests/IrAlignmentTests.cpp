#include "dsp/IrAlignment.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

namespace
{
    // A buffer that is silent up to `onsetSample`, then a short decaying
    // "transient" - close enough to a real cabinet IR's shape for onset
    // detection purposes.
    juce::AudioBuffer<float> makeBufferWithOnsetAt (int onsetSample, int totalSamples = 32)
    {
        juce::AudioBuffer<float> buffer (1, totalSamples);
        buffer.clear();

        for (int i = onsetSample; i < totalSamples; ++i)
            buffer.setSample (0, i, std::pow (0.7f, static_cast<float> (i - onsetSample)));

        return buffer;
    }
}

TEST_CASE ("detectOnsetSample finds a delayed transient's start", "[dsp][ir-alignment]")
{
    const auto buffer = makeBufferWithOnsetAt (10);
    CHECK (IrAlignment::detectOnsetSample (buffer) == 10);
}

TEST_CASE ("detectOnsetSample returns 0 for a transient starting at sample 0", "[dsp][ir-alignment]")
{
    const auto buffer = makeBufferWithOnsetAt (0);
    CHECK (IrAlignment::detectOnsetSample (buffer) == 0);
}

TEST_CASE ("detectOnsetSample returns 0 for a silent buffer", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> silent (1, 16);
    silent.clear();

    CHECK (IrAlignment::detectOnsetSample (silent) == 0);
}

TEST_CASE ("detectOnsetSample returns 0 for an empty buffer", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> empty (1, 0);
    CHECK (IrAlignment::detectOnsetSample (empty) == 0);
}

TEST_CASE ("shiftBySamples with a positive shift prepends silence and preserves content", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> buffer (1, 4);
    for (int i = 0; i < 4; ++i)
        buffer.setSample (0, i, static_cast<float> (i + 1));

    const auto shifted = IrAlignment::shiftBySamples (buffer, 3);

    REQUIRE (shifted.getNumSamples() == 7);
    CHECK (shifted.getSample (0, 0) == Catch::Approx (0.0f));
    CHECK (shifted.getSample (0, 1) == Catch::Approx (0.0f));
    CHECK (shifted.getSample (0, 2) == Catch::Approx (0.0f));
    CHECK (shifted.getSample (0, 3) == Catch::Approx (1.0f));
    CHECK (shifted.getSample (0, 4) == Catch::Approx (2.0f));
    CHECK (shifted.getSample (0, 5) == Catch::Approx (3.0f));
    CHECK (shifted.getSample (0, 6) == Catch::Approx (4.0f));
}

TEST_CASE ("shiftBySamples with a negative shift drops leading samples", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> buffer (1, 4);
    for (int i = 0; i < 4; ++i)
        buffer.setSample (0, i, static_cast<float> (i + 1));

    const auto shifted = IrAlignment::shiftBySamples (buffer, -2);

    REQUIRE (shifted.getNumSamples() == 2);
    CHECK (shifted.getSample (0, 0) == Catch::Approx (3.0f));
    CHECK (shifted.getSample (0, 1) == Catch::Approx (4.0f));
}

TEST_CASE ("shiftBySamples with zero shift returns an unmodified copy", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> buffer (1, 4);
    for (int i = 0; i < 4; ++i)
        buffer.setSample (0, i, static_cast<float> (i + 1));

    const auto shifted = IrAlignment::shiftBySamples (buffer, 0);

    REQUIRE (shifted.getNumSamples() == 4);
    for (int i = 0; i < 4; ++i)
        CHECK (shifted.getSample (0, i) == Catch::Approx (buffer.getSample (0, i)));
}

TEST_CASE ("shiftBySamples with an oversized negative shift clamps to at least one sample", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> buffer (1, 4);
    for (int i = 0; i < 4; ++i)
        buffer.setSample (0, i, static_cast<float> (i + 1));

    const auto shifted = IrAlignment::shiftBySamples (buffer, -100);

    REQUIRE (shifted.getNumSamples() == 1);
    CHECK (shifted.getSample (0, 0) == Catch::Approx (4.0f)); // the last surviving sample
}

TEST_CASE ("alignOnsetToReference shifts a target IR so its onset matches the reference's, same sample rate", "[dsp][ir-alignment]")
{
    constexpr double sampleRate = 48000.0;

    // Reference (IR A) onset: sample 5. Target (IR B) onset: sample 20.
    const auto target = makeBufferWithOnsetAt (20, 64);

    const auto aligned = IrAlignment::alignOnsetToReference (target, sampleRate, 5, sampleRate);

    // The target must be advanced by (20 - 5) = 15 samples, so its onset now
    // lands at sample 5, matching the reference.
    CHECK (IrAlignment::detectOnsetSample (aligned) == 5);
}

TEST_CASE ("alignOnsetToReference handles differing sample rates by aligning in time, not raw samples", "[dsp][ir-alignment]")
{
    // Reference at 48 kHz, onset at sample 480 (10 ms). Target at 96 kHz,
    // onset at sample 480 (5 ms) - the same *sample index* but a different
    // *time*, so a naive sample-domain alignment would get this wrong.
    const auto target = makeBufferWithOnsetAt (480, 2000);

    const auto aligned = IrAlignment::alignOnsetToReference (target, 96000.0, 480, 48000.0);

    // Target needs to be delayed by 5 ms (10ms - 5ms) = 480 samples at its
    // own (96 kHz) rate, landing its onset at sample 960.
    CHECK (IrAlignment::detectOnsetSample (aligned) == 960);
}

TEST_CASE ("alignOnsetToReference is a no-op in onset terms when already aligned", "[dsp][ir-alignment]")
{
    constexpr double sampleRate = 44100.0;
    const auto target = makeBufferWithOnsetAt (12, 64);

    const auto aligned = IrAlignment::alignOnsetToReference (target, sampleRate, 12, sampleRate);

    CHECK (IrAlignment::detectOnsetSample (aligned) == 12);
}

//==============================================================================
// v0.3.0 Precise mode: cross-correlation, sub-sample refinement, and automatic
// polarity detection.

#include "TestHelpers.h"

#include <random>
#include <vector>

namespace
{
    // A cabinet-like IR with a real waveform to correlate against, rather than
    // the pure exponential the Legacy tests above use (which has no structure
    // a cross-correlation could lock onto beyond its onset).
    juce::AudioBuffer<float> makeCorrelatableIr (int numSamples,
                                                  unsigned int seed,
                                                  double sampleRate = 48000.0)
    {
        std::mt19937 engine (seed);
        std::uniform_real_distribution<float> distribution (-1.0f, 1.0f);

        juce::AudioBuffer<float> buffer (1, numSamples);
        buffer.clear();

        auto* data = buffer.getWritePointer (0);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto t = static_cast<float> (i);
            const auto decay = std::exp (-8.0f * t / static_cast<float> (numSamples));

            data[i] = decay * (0.5f * distribution (engine)
                                + 0.4f * std::sin (juce::MathConstants<float>::twoPi * 120.0f
                                                    * t / static_cast<float> (sampleRate)));
        }

        data[0] += 1.0f;

        return buffer;
    }

    // Delays by a fractional number of samples using windowed-sinc
    // interpolation - the reference construction, independent of the
    // Lagrange-3 kernel the implementation uses, so the test cannot pass by
    // sharing a bug with the code.
    juce::AudioBuffer<float> sincDelayed (const juce::AudioBuffer<float>& input, double shiftSamples)
    {
        constexpr int halfWidth = 32;

        const auto numSamples = input.getNumSamples();

        juce::AudioBuffer<float> output (input.getNumChannels(), numSamples);
        output.clear();

        for (int channel = 0; channel < input.getNumChannels(); ++channel)
        {
            const auto* source = input.getReadPointer (channel);
            auto* destination = output.getWritePointer (channel);

            for (int n = 0; n < numSamples; ++n)
            {
                const auto position = static_cast<double> (n) - shiftSamples;
                const auto centre = static_cast<int> (std::floor (position));

                double accumulator = 0.0;

                for (int k = centre - halfWidth; k <= centre + halfWidth; ++k)
                {
                    if (k < 0 || k >= numSamples)
                        continue;

                    const auto x = position - static_cast<double> (k);

                    double sinc = 1.0;

                    if (std::abs (x) > 1.0e-12)
                        sinc = std::sin (juce::MathConstants<double>::pi * x)
                                / (juce::MathConstants<double>::pi * x);

                    const auto w = (x + halfWidth) / (2.0 * halfWidth);
                    const auto window = 0.42 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * w)
                                         + 0.08 * std::cos (2.0 * juce::MathConstants<double>::twoPi * w);

                    accumulator += source[k] * sinc * window;
                }

                destination[n] = static_cast<float> (accumulator);
            }
        }

        return output;
    }

    // Energy of the 50/50 sum of two IRs - the quantity that collapses when
    // polarity or timing is wrong, and the reason alignment exists at all.
    double blendEnergy (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        const auto length = juce::jmax (a.getNumSamples(), b.getNumSamples());

        double energy = 0.0;

        for (int i = 0; i < length; ++i)
        {
            const auto sampleA = i < a.getNumSamples() ? a.getSample (0, i) : 0.0f;
            const auto sampleB = i < b.getNumSamples() ? b.getSample (0, i) : 0.0f;
            const auto sum = 0.5 * (static_cast<double> (sampleA) + sampleB);

            energy += sum * sum;
        }

        return energy;
    }
}

TEST_CASE ("Precise alignment recovers integer and fractional offsets", "[dsp][ir-alignment][precise]")
{
    constexpr int irLength = 4096;
    constexpr double sampleRate = 48000.0;

    const auto reference = makeCorrelatableIr (irLength, 4242u);

    const auto trueShift = GENERATE (7.0, 37.25, 0.5);

    CAPTURE (trueShift);

    const auto target = sincDelayed (reference, trueShift);

    const auto measurement = IrAlignment::measure (reference, sampleRate, target, sampleRate);

    CAPTURE (measurement.lagSamples, measurement.normalisedPeak, measurement.polarityInverted);

    // A shifted copy correlates almost perfectly with its source.
    CHECK (measurement.normalisedPeak > 0.8f);
    CHECK_FALSE (measurement.polarityInverted);

    // Sub-sample accuracy: the parabolic refinement is what gets a fractional
    // offset like 37.25 right, where the v0.2 onset detector could only ever
    // report whole samples.
    CHECK (measurement.lagSamples == Catch::Approx (trueShift).margin (0.1));
}

TEST_CASE ("Precise alignment detects and corrects inverted polarity", "[dsp][ir-alignment][precise]")
{
    constexpr int irLength = 4096;
    constexpr double sampleRate = 48000.0;

    const auto reference = makeCorrelatableIr (irLength, 31337u);

    // A polarity-inverted, slightly delayed capture - the classic
    // rear-of-cabinet mic, or a mispatched preamp.
    const auto inverted = IrAlignment::invertPolarity (sincDelayed (reference, 11.0));

    const auto measurement = IrAlignment::measure (reference, sampleRate, inverted, sampleRate);

    CAPTURE (measurement.lagSamples, measurement.normalisedPeak);

    CHECK (measurement.polarityInverted);
    CHECK (measurement.lagSamples == Catch::Approx (11.0).margin (0.1));

    // The audible consequence: blending the raw inverted capture with the
    // reference cancels; aligning it first restores the sum.
    const auto unaligned = blendEnergy (reference, inverted);

    const auto aligned = IrAlignment::alignToReference (inverted, sampleRate,
                                                        reference, 0, sampleRate,
                                                        IrAlignment::Mode::Precise);

    const auto alignedEnergy = blendEnergy (reference, aligned);

    // Same-polarity, time-aligned reference case to measure against.
    const auto ideal = blendEnergy (reference, reference);

    const auto lossUnalignedDb = juce::Decibels::gainToDecibels (
        static_cast<float> (std::sqrt (unaligned / ideal)), -200.0f);
    const auto lossAlignedDb = juce::Decibels::gainToDecibels (
        static_cast<float> (std::sqrt (alignedEnergy / ideal)), -200.0f);

    CAPTURE (lossUnalignedDb, lossAlignedDb);

    // Unflipped, the blend collapses; corrected, it lands on the ideal.
    //
    // Measured here: -5.8 dB unflipped, 0.0 dB after correction. The
    // cancellation is deep but not total because the inverted capture is also
    // delayed by 11 samples, so the two waveforms are not exact negatives of
    // each other - which is precisely the realistic case (a rear-of-cabinet
    // mic is both flipped and further away). A pure 0-sample inversion would
    // null completely; this bound is set against what the realistic case
    // actually measures rather than against the ideal one.
    CHECK (lossUnalignedDb < -5.0f);
    CHECK (std::abs (lossAlignedDb) < 0.5f);
}

TEST_CASE ("Legacy alignment mode is bit-identical to the v0.2 path", "[dsp][ir-alignment][precise]")
{
    constexpr double sampleRate = 48000.0;

    // The regression snapshot: whatever alignToReference does in Legacy mode
    // must be exactly what alignOnsetToReference (unchanged since v0.2) does,
    // sample for sample - the v1 -> v2 state migration writes Legacy into every
    // upgraded session, so this is what keeps those sessions sounding the same.
    const auto target = makeBufferWithOnsetAt (37, 512);
    const auto reference = makeCorrelatableIr (512, 77u);

    const auto expected = IrAlignment::alignOnsetToReference (target, sampleRate, 12, sampleRate);

    const auto actual = IrAlignment::alignToReference (target, sampleRate,
                                                       reference, 12, sampleRate,
                                                       IrAlignment::Mode::Legacy);

    REQUIRE (actual.getNumChannels() == expected.getNumChannels());
    REQUIRE (actual.getNumSamples() == expected.getNumSamples());

    for (int channel = 0; channel < expected.getNumChannels(); ++channel)
        for (int i = 0; i < expected.getNumSamples(); ++i)
            REQUIRE (actual.getSample (channel, i) == expected.getSample (channel, i));
}

TEST_CASE ("Precise alignment falls back when the IRs do not correlate", "[dsp][ir-alignment][precise]")
{
    constexpr double sampleRate = 48000.0;

    // Two unrelated noise buffers: any correlation peak is an accident of the
    // noise, so trusting it would shift IR B by a meaningless amount.
    const auto reference = makeCorrelatableIr (2048, 1u);

    juce::AudioBuffer<float> silent (1, 2048);
    silent.clear();

    const auto measurement = IrAlignment::measure (reference, sampleRate, silent, sampleRate);

    CHECK (measurement.normalisedPeak == Catch::Approx (0.0f).margin (1.0e-6));

    // Falling back means returning the Legacy result, not something arbitrary.
    const auto expected = IrAlignment::alignOnsetToReference (silent, sampleRate, 5, sampleRate);
    const auto actual = IrAlignment::alignToReference (silent, sampleRate,
                                                       reference, 5, sampleRate,
                                                       IrAlignment::Mode::Precise);

    REQUIRE (actual.getNumSamples() == expected.getNumSamples());

    for (int i = 0; i < expected.getNumSamples(); ++i)
        REQUIRE (actual.getSample (0, i) == expected.getSample (0, i));
}

TEST_CASE ("Fractional shifting is exact for whole-sample amounts", "[dsp][ir-alignment][precise]")
{
    const auto buffer = makeCorrelatableIr (256, 9u);

    // A whole-sample shift must bypass the interpolator entirely - otherwise
    // every integer alignment would pointlessly lose high frequencies.
    const auto fractional = IrAlignment::shiftByFractionalSamples (buffer, 5.0f);
    const auto integer = IrAlignment::shiftBySamples (buffer, 5);

    REQUIRE (fractional.getNumSamples() == integer.getNumSamples());

    for (int i = 0; i < integer.getNumSamples(); ++i)
        REQUIRE (fractional.getSample (0, i) == integer.getSample (0, i));
}
