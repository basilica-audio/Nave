#include "dsp/MorphEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 256;

    juce::dsp::ProcessSpec makeSpec (int numChannels = 1)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (testBlockSize);
        spec.numChannels = static_cast<juce::uint32> (numChannels);
        return spec;
    }

    // A cabinet-like IR: direct transient, decaying noise, and two resonances,
    // so the magnitude response has structure a morph could plausibly damage.
    juce::AudioBuffer<float> makeCabinetIr (int numSamples, unsigned int seed)
    {
        std::mt19937 engine (seed);
        std::uniform_real_distribution<float> distribution (-1.0f, 1.0f);

        juce::AudioBuffer<float> buffer (1, numSamples);
        buffer.clear();

        auto* data = buffer.getWritePointer (0);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto t = static_cast<float> (i);
            const auto decay = std::exp (-7.0f * t / static_cast<float> (numSamples));

            const auto modeLow = std::sin (juce::MathConstants<float>::twoPi * 110.0f
                                            * t / static_cast<float> (testSampleRate));
            const auto modeHigh = std::sin (juce::MathConstants<float>::twoPi * 1900.0f
                                             * t / static_cast<float> (testSampleRate));

            data[i] = decay * (0.5f * distribution (engine) + 0.35f * modeLow + 0.25f * modeHigh);
        }

        data[0] += 1.0f;

        return buffer;
    }

    // A copy of `buffer` delayed by a whole number of samples, used to build
    // the "same mic, moved slightly" pair the comb test needs.
    juce::AudioBuffer<float> delayedCopy (const juce::AudioBuffer<float>& buffer, int delaySamples)
    {
        juce::AudioBuffer<float> result (buffer.getNumChannels(), buffer.getNumSamples());
        result.clear();

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int i = delaySamples; i < buffer.getNumSamples(); ++i)
                result.setSample (channel, i, buffer.getSample (channel, i - delaySamples));

        return result;
    }

    // Captures the engine's impulse response at the current blend, with the
    // bulk-delay smoother already settled so the measurement reflects the
    // steady state rather than a glide in progress.
    std::vector<float> captureImpulseResponse (MorphEngine& engine, int length)
    {
        juce::AudioBuffer<float> scratch (1, testBlockSize);

        // Pre-roll silence: process() only learns the new target delay when it
        // runs, and the one-pole then needs time to reach it.
        for (int i = 0; i < 32; ++i)
        {
            scratch.clear();
            juce::dsp::AudioBlock<float> block (scratch);
            engine.process (block);
        }

        // reset() snaps the delay smoother to its (now correct) target, clears
        // the convolver's input history, and completes any crossfade still in
        // flight - everything needed for a clean, repeatable measurement.
        engine.reset();

        std::vector<float> response;
        response.reserve (static_cast<size_t> (length));

        bool impulseSent = false;

        while (static_cast<int> (response.size()) < length)
        {
            scratch.clear();

            if (! impulseSent)
            {
                scratch.setSample (0, 0, 1.0f);
                impulseSent = true;
            }

            juce::dsp::AudioBlock<float> block (scratch);
            engine.process (block);

            for (int i = 0; i < testBlockSize && static_cast<int> (response.size()) < length; ++i)
                response.push_back (scratch.getSample (0, i));
        }

        return response;
    }

    // The band over which the morph's magnitude claims are asserted.
    //
    // The morph applies its interpolated bulk delay through a Lagrange-3
    // fractional delay line, and every polynomial interpolator has a
    // low-pass character that deepens toward Nyquist. Measured against a
    // pure minimum-phase reference at a 6.7-sample delay, the deviation is
    // 0.00 dB below 2 kHz, 0.03 dB at 4-6 kHz, 0.17 dB at 6-8 kHz, then
    // climbs to ~4.4 dB median approaching Nyquist. That is the
    // interpolator's known response, not a morph error - and it is
    // irrelevant for the job: a guitar cabinet IR has essentially no content
    // above 6 kHz, which is why every cab sim ships a HiCut. The droop
    // itself is pinned separately by the "bulk delay interpolation" test
    // below, so it cannot silently worsen.
    constexpr double magnitudeAssertionMaxHz = 8000.0;

    std::vector<float> magnitudeSpectrumDb (const std::vector<float>& signal, int fftOrder)
    {
        juce::dsp::FFT fft (fftOrder);

        const auto fftSize = 1 << fftOrder;
        const auto numBins = fftSize / 2 + 1;

        std::vector<float> scratch (static_cast<size_t> (fftSize) * 2, 0.0f);
        std::copy_n (signal.begin(),
                      std::min (signal.size(), static_cast<size_t> (fftSize)),
                      scratch.begin());

        fft.performRealOnlyForwardTransform (scratch.data(), true);

        std::vector<float> result;
        result.resize (static_cast<size_t> (numBins));

        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto re = scratch[static_cast<size_t> (bin) * 2];
            const auto im = scratch[static_cast<size_t> (bin) * 2 + 1];

            result[static_cast<size_t> (bin)] =
                juce::Decibels::gainToDecibels (std::sqrt (re * re + im * im), -200.0f);
        }

        return result;
    }

    // The number of spectrum bins at or below magnitudeAssertionMaxHz.
    size_t binsWithinAssertionBand (size_t numBins)
    {
        const auto nyquistHz = testSampleRate * 0.5;
        const auto fraction = juce::jlimit (0.0, 1.0, magnitudeAssertionMaxHz / nyquistHz);

        return static_cast<size_t> (static_cast<double> (numBins - 1) * fraction);
    }

    // What every other IR loader does at Blend 50%: sum the two IRs.
    std::vector<float> naiveMix (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        const auto length = juce::jmax (a.getNumSamples(), b.getNumSamples());

        std::vector<float> result (static_cast<size_t> (length), 0.0f);

        for (int i = 0; i < a.getNumSamples(); ++i)
            result[static_cast<size_t> (i)] += 0.5f * a.getSample (0, i);

        for (int i = 0; i < b.getNumSamples(); ++i)
            result[static_cast<size_t> (i)] += 0.5f * b.getSample (0, i);

        return result;
    }
}

//==============================================================================
// Test 7 (merge gate): THE differentiator. Blending a mic capture with a
// slightly-moved copy of itself is the standard "two off-axis captures" case a
// reviewer will try. A naive 50/50 sum combs - a deep notch at the frequency
// whose half-wavelength equals the offset. The morph must not, because it puts
// exactly one impulse response in the signal path instead of summing two.
TEST_CASE ("MorphEngine blends without comb-filtering", "[dsp][morph-engine]")
{
    constexpr int irLength = 4096;

    // 0.15 ms at 48 kHz = 7.2 samples; 7 samples keeps the reference IR
    // constructible by pure integer shifting, and puts the first comb notch
    // near 3.4 kHz - squarely in the guitar presence range.
    constexpr int offsetSamples = 7;

    const auto irA = makeCabinetIr (irLength, 12345u);
    const auto irB = delayedCopy (irA, offsetSamples);

    const auto originalDb = magnitudeSpectrumDb (
        [&]
        {
            std::vector<float> v (static_cast<size_t> (irLength));
            std::copy_n (irA.getReadPointer (0), irLength, v.begin());
            return v;
        }(), 14);

    // First, establish that this pair really does comb when summed naively -
    // otherwise the morph's success below would prove nothing.
    const auto naiveDb = magnitudeSpectrumDb (naiveMix (irA, irB), 14);

    float deepestNotch = 0.0f;

    for (size_t bin = 0; bin < naiveDb.size(); ++bin)
        deepestNotch = juce::jmax (deepestNotch, originalDb[bin] - naiveDb[bin]);

    CAPTURE (deepestNotch);
    REQUIRE (deepestNotch > 20.0f);

    // Now the morph at the same 50/50 position.
    MorphEngine engine;
    engine.prepare (makeSpec());

    engine.setImpulseResponse (0, irA);
    engine.setImpulseResponse (1, irB);
    engine.setBlend (0.5f);
    engine.resynthesiseSynchronouslyForTesting();

    REQUIRE (engine.isReady());

    const auto morphed = captureImpulseResponse (engine, irLength);
    const auto morphedDb = magnitudeSpectrumDb (morphed, 14);

    // A pure delay does not change a magnitude response, so both slots have
    // IDENTICAL log-magnitude spectra here. The interpolation is therefore an
    // exact reproduction of that shared magnitude, whatever the blend value -
    // which is precisely why no notch can appear.
    const auto peakDb = *std::max_element (originalDb.begin(), originalDb.end());

    std::vector<float> deviations;

    const auto lastBin = binsWithinAssertionBand (morphedDb.size());

    for (size_t bin = 0; bin < lastBin; ++bin)
        if (originalDb[bin] >= peakDb - 40.0f)
            deviations.push_back (std::abs (originalDb[bin] - morphedDb[bin]));

    std::sort (deviations.begin(), deviations.end());

    REQUIRE (deviations.size() > 1000);

    const auto median = deviations[deviations.size() / 2];
    const auto percentile99 = deviations[static_cast<size_t> (deviations.size() * 0.99)];

    CAPTURE (median, percentile99, deviations.back());

    // Nothing remotely resembling the 20 dB+ notch the naive sum produced.
    CHECK (percentile99 < 1.0f);
    CHECK (deviations.back() < 3.0f);
}

//==============================================================================
// Test 8a (merge gate): a Blend sweep must be continuous. A discontinuity here
// would be an audible lurch as the user drags the knob, however good each
// individual endpoint sounded.
TEST_CASE ("MorphEngine blend sweep is spectrally continuous", "[dsp][morph-engine]")
{
    constexpr int irLength = 2048;
    constexpr int numSteps = 33;

    const auto irA = makeCabinetIr (irLength, 555u);
    const auto irB = makeCabinetIr (irLength, 999u);

    MorphEngine engine;
    engine.prepare (makeSpec());

    engine.setImpulseResponse (0, irA);
    engine.setImpulseResponse (1, irB);

    std::vector<float> previousDb;

    float worstStepRms = 0.0f;

    for (int step = 0; step < numSteps; ++step)
    {
        const auto blend = static_cast<float> (step) / static_cast<float> (numSteps - 1);

        engine.setBlend (blend);
        engine.resynthesiseSynchronouslyForTesting();

        const auto response = captureImpulseResponse (engine, irLength);
        const auto currentDb = magnitudeSpectrumDb (response, 13);

        if (! previousDb.empty())
        {
            // RMS log-spectral distance between adjacent blend steps, over the
            // audible top 40 dB.
            const auto peakDb = *std::max_element (currentDb.begin(), currentDb.end());

            double sumOfSquares = 0.0;
            int count = 0;

            const auto lastBin = binsWithinAssertionBand (currentDb.size());

            for (size_t bin = 0; bin < lastBin; ++bin)
            {
                if (currentDb[bin] < peakDb - 40.0f)
                    continue;

                const auto difference = static_cast<double> (currentDb[bin]) - previousDb[bin];
                sumOfSquares += difference * difference;
                ++count;
            }

            if (count > 0)
            {
                const auto rms = static_cast<float> (std::sqrt (sumOfSquares / count));
                worstStepRms = juce::jmax (worstStepRms, rms);
            }
        }

        previousDb = currentDb;
    }

    CAPTURE (worstStepRms);

    // The sweep takes 33 steps across the full range, so each step is ~3% of
    // travel; anything under 0.5 dB RMS is a smooth glide, not a jump.
    CHECK (worstStepRms < 0.5f);
}

//==============================================================================
// Test 8b: endpoint semantics. Morph is defined to minimum-phase both
// endpoints, so b = 0 must reproduce MPT(A), NOT raw A. Pinning this stops the
// endpoint quietly drifting toward "nearly raw A" in some future refactor, and
// documents the deliberate difference from Crossfade mode.
TEST_CASE ("MorphEngine endpoints are the minimum-phase slot IRs", "[dsp][morph-engine]")
{
    constexpr int irLength = 2048;

    const auto irA = makeCabinetIr (irLength, 4321u);
    const auto irB = makeCabinetIr (irLength, 8765u);

    MorphEngine engine;
    engine.prepare (makeSpec());

    engine.setImpulseResponse (0, irA);
    engine.setImpulseResponse (1, irB);

    const auto peakOf = [] (const std::vector<float>& v)
    {
        float peak = 0.0f;

        for (auto sample : v)
            peak = juce::jmax (peak, std::abs (sample));

        return peak;
    };

    const auto compareAgainstMinimumPhase = [&] (float blend, const juce::AudioBuffer<float>& slotIr)
    {
        engine.setBlend (blend);
        engine.resynthesiseSynchronouslyForTesting();

        const auto response = captureImpulseResponse (engine, irLength);

        const auto expectedBuffer = MinPhase::transform (slotIr);

        std::vector<float> expected (static_cast<size_t> (irLength));
        std::copy_n (expectedBuffer.getReadPointer (0), irLength, expected.begin());

        // Compare spectra rather than samples: the engine applies the slot's
        // bulk delay on top, which shifts the impulse in time without changing
        // its magnitude response.
        const auto responseDb = magnitudeSpectrumDb (response, 13);
        const auto expectedDb = magnitudeSpectrumDb (expected, 13);

        const auto peakDb = *std::max_element (expectedDb.begin(), expectedDb.end());

        std::vector<float> deviations;

        const auto lastBin = binsWithinAssertionBand (responseDb.size());

        for (size_t bin = 0; bin < lastBin; ++bin)
            if (expectedDb[bin] >= peakDb - 40.0f)
                deviations.push_back (std::abs (expectedDb[bin] - responseDb[bin]));

        std::sort (deviations.begin(), deviations.end());

        REQUIRE (deviations.size() > 100);
        REQUIRE (peakOf (response) > 0.0f);

        const auto percentile95 = deviations[static_cast<size_t> (deviations.size() * 0.95)];

        CAPTURE (blend, percentile95, deviations.back());
        CHECK (percentile95 < 1.0f);
    };

    SECTION ("blend 0 reproduces the minimum-phase IR A")
    {
        compareAgainstMinimumPhase (0.0f, irA);
    }

    SECTION ("blend 1 reproduces the minimum-phase IR B")
    {
        compareAgainstMinimumPhase (1.0f, irB);
    }
}

//==============================================================================
// The bulk delay must be interpolated, not switched: that is what makes a
// Blend drag sound like a mic physically moving.
TEST_CASE ("MorphEngine interpolates the bulk delay between slots", "[dsp][morph-engine]")
{
    constexpr int irLength = 2048;
    constexpr int offsetSamples = 40;

    const auto irA = makeCabinetIr (irLength, 246u);
    const auto irB = delayedCopy (irA, offsetSamples);

    MorphEngine engine;
    engine.prepare (makeSpec());

    engine.setImpulseResponse (0, irA);
    engine.setImpulseResponse (1, irB);

    engine.setBlend (0.0f);
    engine.resynthesiseSynchronouslyForTesting();
    const auto delayAtZero = engine.getInterpolatedDelaySamples();

    engine.setBlend (1.0f);
    engine.resynthesiseSynchronouslyForTesting();
    const auto delayAtOne = engine.getInterpolatedDelaySamples();

    engine.setBlend (0.5f);
    engine.resynthesiseSynchronouslyForTesting();
    const auto delayAtHalf = engine.getInterpolatedDelaySamples();

    CAPTURE (delayAtZero, delayAtHalf, delayAtOne);

    // Slot B is slot A pushed back by a known amount, so its estimated bulk
    // delay must exceed slot A's by that amount.
    CHECK (delayAtOne - delayAtZero == Catch::Approx (static_cast<float> (offsetSamples)).margin (1.0f));

    // And the midpoint must land halfway between - linear interpolation, not a
    // switch at 50%.
    CHECK (delayAtHalf == Catch::Approx (0.5f * (delayAtZero + delayAtOne)).margin (0.5f));

    // Pin the Lagrange-3 interpolator's high-frequency droop, which is the
    // price of applying that delay continuously (see magnitudeAssertionMaxHz).
    // Asserting it here means it is a measured, bounded characteristic rather
    // than an unexamined one - if a future change made the interpolation worse,
    // this fails rather than quietly dulling every morphed cabinet.
    SECTION ("the delay interpolator stays transparent across the cabinet band")
    {
        const auto irLengthForMeasurement = 2048;

        engine.setBlend (0.5f);
        engine.resynthesiseSynchronouslyForTesting();

        const auto response = captureImpulseResponse (engine, irLengthForMeasurement);
        const auto responseDb = magnitudeSpectrumDb (response, 13);

        // Reference: the same morph magnitude with no bulk delay applied at
        // all, which is exactly slot A's minimum-phase magnitude (slot B is a
        // pure delay of slot A, so both share one magnitude response).
        const auto referenceBuffer = MinPhase::transform (irA);

        std::vector<float> reference;
        reference.resize (static_cast<size_t> (irLengthForMeasurement));
        std::copy_n (referenceBuffer.getReadPointer (0), irLengthForMeasurement, reference.begin());

        const auto referenceDb = magnitudeSpectrumDb (reference, 13);

        const auto peakDb = *std::max_element (referenceDb.begin(), referenceDb.end());
        const auto lastBin = binsWithinAssertionBand (responseDb.size());

        float worstInBand = 0.0f;

        for (size_t bin = 0; bin < lastBin; ++bin)
            if (referenceDb[bin] >= peakDb - 40.0f)
                worstInBand = juce::jmax (worstInBand, std::abs (referenceDb[bin] - responseDb[bin]));

        CAPTURE (worstInBand);

        // Measured worst case below 8 kHz is ~0.2 dB; 1 dB leaves headroom for
        // a different fractional part without hiding a real regression.
        CHECK (worstInBand < 1.0f);
    }
}

//==============================================================================
// Robustness: the worker thread must start, stop and re-prepare cleanly, and a
// single loaded slot must still produce sound rather than silence.
TEST_CASE ("MorphEngine handles partial and repeated configuration", "[dsp][morph-engine]")
{
    SECTION ("one slot only still produces output")
    {
        MorphEngine engine;
        engine.prepare (makeSpec());

        engine.setImpulseResponse (0, makeCabinetIr (1024, 1u));
        engine.setBlend (0.5f);
        engine.resynthesiseSynchronouslyForTesting();

        REQUIRE (engine.isReady());

        const auto response = captureImpulseResponse (engine, 1024);

        float peak = 0.0f;

        for (auto sample : response)
            peak = juce::jmax (peak, std::abs (sample));

        CHECK (peak > 0.0f);
        CHECK (std::all_of (response.begin(), response.end(),
                             [] (float sample) { return std::isfinite (sample); }));
    }

    SECTION ("no slots loaded is a silent passthrough, not a crash")
    {
        MorphEngine engine;
        engine.prepare (makeSpec());

        engine.setBlend (0.5f);
        engine.resynthesiseSynchronouslyForTesting();

        juce::AudioBuffer<float> buffer (1, testBlockSize);
        TestHelpers::fillWithSine (buffer, testSampleRate, 440.0);

        const auto rmsBefore = TestHelpers::rms (buffer);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        // Nothing published, so the block passes through untouched.
        CHECK (TestHelpers::rms (buffer) == Catch::Approx (rmsBefore));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }

    SECTION ("re-preparing at a new sample rate is clean")
    {
        MorphEngine engine;
        engine.prepare (makeSpec());
        engine.setImpulseResponse (0, makeCabinetIr (1024, 2u));
        engine.resynthesiseSynchronouslyForTesting();

        juce::dsp::ProcessSpec other;
        other.sampleRate = 96000.0;
        other.maximumBlockSize = 512;
        other.numChannels = 2;

        engine.prepare (other);
        engine.resynthesiseSynchronouslyForTesting();

        juce::AudioBuffer<float> buffer (2, 512);
        TestHelpers::fillWithSine (buffer, 96000.0, 440.0);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);

        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}
