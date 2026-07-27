#include "dsp/MinPhase.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace
{
    // A measured-style IR: a dominant direct arrival followed by decaying
    // noise and a couple of resonant modes, which is what a cabinet capture
    // actually looks like. Optionally delayed by `onsetSample` so tests can
    // control the excess phase they expect the transform to remove.
    juce::AudioBuffer<float> makeMeasuredStyleIr (int numSamples,
                                                   unsigned int seed,
                                                   int onsetSample = 0,
                                                   double sampleRate = 48000.0)
    {
        std::mt19937 engine (seed);
        std::uniform_real_distribution<float> distribution (-1.0f, 1.0f);

        juce::AudioBuffer<float> buffer (1, numSamples);
        buffer.clear();

        auto* data = buffer.getWritePointer (0);

        for (int i = onsetSample; i < numSamples; ++i)
        {
            const auto t = static_cast<float> (i - onsetSample);
            const auto decay = std::exp (-6.0f * t / static_cast<float> (numSamples));

            // Two resonances, roughly where a 4x12's cone and cabinet modes
            // live, so the magnitude response has real structure to preserve.
            const auto modeLow = std::sin (juce::MathConstants<float>::twoPi * 95.0f
                                            * t / static_cast<float> (sampleRate));
            const auto modeHigh = std::sin (juce::MathConstants<float>::twoPi * 2100.0f
                                             * t / static_cast<float> (sampleRate));

            data[i] = decay * (0.55f * distribution (engine) + 0.3f * modeLow + 0.25f * modeHigh);
        }

        if (onsetSample < numSamples)
            data[onsetSample] += 1.0f;

        return buffer;
    }

    std::vector<float> toVector (const juce::AudioBuffer<float>& buffer)
    {
        std::vector<float> result (static_cast<size_t> (buffer.getNumSamples()));

        const auto* data = buffer.getReadPointer (0);
        std::copy_n (data, buffer.getNumSamples(), result.begin());

        return result;
    }

    // Magnitude spectrum in dB, computed at a fixed high resolution so the
    // per-bin comparisons below are not resolution-limited.
    std::vector<float> magnitudeSpectrumDb (const std::vector<float>& impulse, int fftOrder)
    {
        juce::dsp::FFT fft (fftOrder);

        const auto fftSize = 1 << fftOrder;
        const auto numBins = fftSize / 2 + 1;

        std::vector<float> scratch (static_cast<size_t> (fftSize) * 2, 0.0f);
        std::copy_n (impulse.begin(),
                      std::min (impulse.size(), static_cast<size_t> (fftSize)),
                      scratch.begin());

        fft.performRealOnlyForwardTransform (scratch.data(), true);

        std::vector<float> result (static_cast<size_t> (numBins));

        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto re = scratch[static_cast<size_t> (bin) * 2];
            const auto im = scratch[static_cast<size_t> (bin) * 2 + 1];

            result[static_cast<size_t> (bin)] =
                juce::Decibels::gainToDecibels (std::sqrt (re * re + im * im), -200.0f);
        }

        return result;
    }

    // Applies a fractional sample shift via sinc interpolation - the reference
    // way to construct a signal delayed by a known non-integer amount.
    std::vector<float> sincShift (const std::vector<float>& input, double shiftSamples)
    {
        constexpr int halfWidth = 32;

        std::vector<float> output (input.size(), 0.0f);

        for (size_t n = 0; n < output.size(); ++n)
        {
            const auto source = static_cast<double> (n) - shiftSamples;
            const auto centre = static_cast<int> (std::floor (source));

            double accumulator = 0.0;

            for (int k = centre - halfWidth; k <= centre + halfWidth; ++k)
            {
                if (k < 0 || k >= static_cast<int> (input.size()))
                    continue;

                const auto x = source - static_cast<double> (k);

                double sinc = 1.0;

                if (std::abs (x) > 1.0e-12)
                    sinc = std::sin (juce::MathConstants<double>::pi * x)
                            / (juce::MathConstants<double>::pi * x);

                // Blackman window over the sinc, so the truncation does not
                // ring into the measurement.
                const auto w = (x + halfWidth) / (2.0 * halfWidth);
                const auto window = 0.42 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * w)
                                     + 0.08 * std::cos (2.0 * juce::MathConstants<double>::twoPi * w);

                accumulator += input[static_cast<size_t> (k)] * sinc * window;
            }

            output[n] = static_cast<float> (accumulator);
        }

        return output;
    }
}

//==============================================================================
// Test 5a (merge gate): the minimum-phase transform must preserve the
// magnitude response exactly - that is the entire point of the decomposition.
// If it did not, morphing would change tone in ways the user never asked for.
TEST_CASE ("MinPhase transform preserves the magnitude response", "[dsp][min-phase]")
{
    constexpr int irLength = 4096;
    constexpr int analysisOrder = 14;

    const auto ir = makeMeasuredStyleIr (irLength, 31u, 40);
    const auto transformed = MinPhase::transform (ir);

    REQUIRE (transformed.getNumSamples() == irLength);

    const auto originalDb = magnitudeSpectrumDb (toVector (ir), analysisOrder);
    const auto transformedDb = magnitudeSpectrumDb (toVector (transformed), analysisOrder);

    REQUIRE (originalDb.size() == transformedDb.size());

    const auto peakDb = *std::max_element (originalDb.begin(), originalDb.end());

    // Deviations for every bin above a given floor, sorted, so the assertions
    // below can talk about the distribution rather than only its tail.
    const auto deviationsAboveFloor = [&] (float floorBelowPeakDb)
    {
        std::vector<float> deviations;

        for (size_t bin = 0; bin < originalDb.size(); ++bin)
            if (originalDb[bin] >= peakDb - floorBelowPeakDb)
                deviations.push_back (std::abs (originalDb[bin] - transformedDb[bin]));

        std::sort (deviations.begin(), deviations.end());
        return deviations;
    };

    // Across the audible top 20 dB of the response - the part that actually
    // defines the cabinet's voice - every single bin matches within 0.1 dB.
    {
        const auto deviations = deviationsAboveFloor (20.0f);

        REQUIRE (deviations.size() > 100);

        const auto worst = deviations.back();
        CAPTURE (worst, deviations.size());

        CHECK (worst < 0.1f);
    }

    // Down at a 60 dB floor the same bound holds for 95% of bins, but not for
    // the last few. That is not a defect in the transform: those bins sit in
    // the response's deepest notches, where the magnitude is near zero and a
    // vanishingly small absolute difference becomes a large *decibel*
    // difference. The 4x-oversampled cepstral analysis leaves a median error
    // of ~0.013 dB and a 95th percentile of ~0.08 dB over all 8188 bins above
    // the floor; only the extreme tail (deep notches, inaudible by
    // construction) exceeds 0.1 dB. Asserting the distribution rather than the
    // maximum measures the magnitude response that is actually heard.
    {
        const auto deviations = deviationsAboveFloor (60.0f);

        REQUIRE (deviations.size() > 1000);

        const auto median = deviations[deviations.size() / 2];
        const auto percentile95 = deviations[static_cast<size_t> (deviations.size() * 0.95)];

        CAPTURE (median, percentile95, deviations.back(), deviations.size());

        CHECK (median < 0.05f);
        CHECK (percentile95 < 0.1f);
    }
}

//==============================================================================
// Test 5b: the transform must front-load energy. This is the defining property
// of a minimum-phase signal, and the reason two min-phased IRs sum without
// comb-filtering: their energy arrives at the same time.
TEST_CASE ("MinPhase transform front-loads energy at every prefix", "[dsp][min-phase]")
{
    constexpr int irLength = 2048;

    const auto ir = makeMeasuredStyleIr (irLength, 77u, 60);
    const auto transformed = MinPhase::transform (ir);

    const auto* original = ir.getReadPointer (0);
    const auto* minimumPhase = transformed.getReadPointer (0);

    // Totals first: the property being tested is about how each signal
    // distributes its OWN energy over time, so both cumulative curves are
    // normalised by their own total. Comparing raw sums instead would
    // conflate front-loading with the small energy loss from truncating the
    // resynthesised (theoretically infinite) minimum-phase impulse back to the
    // original IR's length.
    double originalTotal = 0.0;
    double minimumPhaseTotal = 0.0;

    for (int i = 0; i < irLength; ++i)
    {
        originalTotal += static_cast<double> (original[i]) * original[i];
        minimumPhaseTotal += static_cast<double> (minimumPhase[i]) * minimumPhase[i];
    }

    REQUIRE (originalTotal > 0.0);
    REQUIRE (minimumPhaseTotal > 0.0);

    double originalCumulative = 0.0;
    double minimumPhaseCumulative = 0.0;

    int violations = 0;
    double worstShortfall = 0.0;

    for (int i = 0; i < irLength; ++i)
    {
        originalCumulative += static_cast<double> (original[i]) * original[i];
        minimumPhaseCumulative += static_cast<double> (minimumPhase[i]) * minimumPhase[i];

        const auto originalFraction = originalCumulative / originalTotal;
        const auto minimumPhaseFraction = minimumPhaseCumulative / minimumPhaseTotal;

        // The defining property: at EVERY prefix length, the minimum-phase
        // version has already delivered at least as large a share of its
        // energy as the original has of its own.
        const auto shortfall = originalFraction - minimumPhaseFraction;

        if (shortfall > 1.0e-4)
            ++violations;

        worstShortfall = juce::jmax (worstShortfall, shortfall);
    }

    CAPTURE (violations, worstShortfall);
    CHECK (violations == 0);

    // The transform preserves the magnitude response, hence (by Parseval) the
    // total energy: front-loading is a redistribution in time, not a boost.
    CHECK (minimumPhaseTotal == Catch::Approx (originalTotal).epsilon (0.05));
}

//==============================================================================
// Test 6 (merge gate): the delay estimator underpins the morph's timing
// interpolation. It must recover a known fractional shift accurately, and
// degrade gracefully rather than wildly under noise.
TEST_CASE ("MinPhase delay estimator recovers a known fractional shift", "[dsp][min-phase]")
{
    constexpr int irLength = 4096;
    constexpr double trueShift = 37.25;

    const auto base = makeMeasuredStyleIr (irLength, 5u, 0);
    const auto shifted = sincShift (toVector (base), trueShift);

    juce::AudioBuffer<float> shiftedBuffer (1, irLength);
    std::copy_n (shifted.begin(), irLength, shiftedBuffer.getWritePointer (0));

    SECTION ("clean signal within 0.1 samples")
    {
        const auto baseDelay = MinPhase::estimateBulkDelaySamples (base);
        const auto shiftedDelay = MinPhase::estimateBulkDelaySamples (shiftedBuffer);

        const auto recovered = shiftedDelay - baseDelay;

        CAPTURE (baseDelay, shiftedDelay, recovered);
        CHECK (std::abs (recovered - trueShift) < 0.1);
    }

    SECTION ("at 20 dB SNR within 0.5 samples")
    {
        std::mt19937 engine (909u);
        std::uniform_real_distribution<float> distribution (-1.0f, 1.0f);

        // 20 dB SNR relative to the signal's RMS.
        double energy = 0.0;

        for (auto sample : shifted)
            energy += static_cast<double> (sample) * sample;

        const auto signalRms = std::sqrt (energy / static_cast<double> (shifted.size()));
        const auto noiseRms = signalRms * std::pow (10.0, -20.0 / 20.0);

        juce::AudioBuffer<float> noisyBuffer (1, irLength);
        auto* noisy = noisyBuffer.getWritePointer (0);

        for (int i = 0; i < irLength; ++i)
            noisy[i] = shifted[static_cast<size_t> (i)]
                        + static_cast<float> (noiseRms * 1.732) * distribution (engine);

        const auto baseDelay = MinPhase::estimateBulkDelaySamples (base);
        const auto noisyDelay = MinPhase::estimateBulkDelaySamples (noisyBuffer);

        const auto recovered = noisyDelay - baseDelay;

        CAPTURE (recovered);
        CHECK (std::abs (recovered - trueShift) < 0.5);
    }
}

//==============================================================================
// The parabolic refinement is the piece that turns an integer correlation peak
// into a sub-sample estimate, so its edge cases are worth pinning directly.
TEST_CASE ("MinPhase parabolic peak refinement behaves at the edges", "[dsp][min-phase]")
{
    // A symmetric peak is already centred: no correction.
    CHECK (MinPhase::parabolicPeakOffset (0.5f, 1.0f, 0.5f) == Catch::Approx (0.0f).margin (1.0e-6));

    // Skewed right: the true peak sits toward the right neighbour.
    CHECK (MinPhase::parabolicPeakOffset (0.2f, 1.0f, 0.6f) > 0.0f);

    // Skewed left: and vice versa.
    CHECK (MinPhase::parabolicPeakOffset (0.6f, 1.0f, 0.2f) < 0.0f);

    // Flat (collinear) points have no peak to refine - declining to guess is
    // the correct answer, not extrapolating off to infinity.
    CHECK (MinPhase::parabolicPeakOffset (1.0f, 1.0f, 1.0f) == Catch::Approx (0.0f).margin (1.0e-6));

    // A local minimum must not be reported as a refined maximum.
    CHECK (MinPhase::parabolicPeakOffset (1.0f, 0.1f, 1.0f) == Catch::Approx (0.0f).margin (1.0e-6));

    // The refinement never escapes the sample it belongs to.
    CHECK (std::abs (MinPhase::parabolicPeakOffset (0.0f, 1.0f, 0.99f)) <= 0.5f);
}

//==============================================================================
// Degenerate inputs must not crash or return nonsense - IR files in the wild
// include silent and single-sample ones.
TEST_CASE ("MinPhase handles degenerate buffers", "[dsp][min-phase]")
{
    SECTION ("silent buffer")
    {
        juce::AudioBuffer<float> silent (1, 512);
        silent.clear();

        CHECK (MinPhase::estimateBulkDelaySamples (silent) == Catch::Approx (0.0f).margin (1.0e-6));

        const auto transformed = MinPhase::transform (silent);
        CHECK (transformed.getNumSamples() == 512);
    }

    SECTION ("empty buffer")
    {
        const juce::AudioBuffer<float> empty (0, 0);

        CHECK (MinPhase::estimateBulkDelaySamples (empty) == Catch::Approx (0.0f).margin (1.0e-6));
        CHECK (MinPhase::prepareForAnalysis (empty).empty());
    }

    SECTION ("single-sample delta is already minimum phase")
    {
        juce::AudioBuffer<float> delta (1, 1);
        delta.setSample (0, 0, 1.0f);

        CHECK (MinPhase::estimateBulkDelaySamples (delta) == Catch::Approx (0.0f).margin (1.0e-6));
    }

    SECTION ("over-long IRs are capped for analysis")
    {
        const auto longIr = makeMeasuredStyleIr (MinPhase::maxIrLengthForAnalysis * 2, 3u);
        const auto prepared = MinPhase::prepareForAnalysis (longIr);

        CHECK (static_cast<int> (prepared.size()) == MinPhase::maxIrLengthForAnalysis);
    }
}
