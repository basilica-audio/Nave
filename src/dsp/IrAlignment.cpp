#include "IrAlignment.h"
#include "MinPhase.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace
{
    // Sums a buffer to mono for correlation analysis. Correlating one channel
    // pair at a time and picking a winner would let a stereo IR's two sides
    // disagree about the shift; a mono sum measures the arrival the listener
    // actually hears, and one common shift is then applied to every channel
    // (preserving the IR's own inter-channel timing, which is part of its
    // character and must not be "corrected").
    std::vector<float> toMono (const juce::AudioBuffer<float>& buffer)
    {
        const auto numChannels = buffer.getNumChannels();
        const auto numSamples = buffer.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return {};

        std::vector<float> mono (static_cast<size_t> (numSamples), 0.0f);

        const auto scale = 1.0f / static_cast<float> (numChannels);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int i = 0; i < numSamples; ++i)
                mono[static_cast<size_t> (i)] += data[i] * scale;
        }

        return mono;
    }

    double energyOf (const std::vector<float>& signal) noexcept
    {
        double energy = 0.0;

        for (auto sample : signal)
            energy += static_cast<double> (sample) * sample;

        return energy;
    }

    // Resamples `signal` from `fromRate` to `toRate` by linear interpolation.
    // Only used to put two IRs captured at different rates on a common time
    // base before correlating; the result feeds a lag measurement, never the
    // audio path, so linear interpolation's accuracy is ample.
    std::vector<float> resampleForAnalysis (const std::vector<float>& signal,
                                             double fromRate,
                                             double toRate)
    {
        if (signal.empty() || fromRate <= 0.0 || toRate <= 0.0
            || std::abs (fromRate - toRate) < 1.0e-9)
            return signal;

        const auto ratio = toRate / fromRate;
        const auto outputLength = static_cast<size_t> (static_cast<double> (signal.size()) * ratio);

        std::vector<float> output (std::max<size_t> (1, outputLength), 0.0f);

        for (size_t i = 0; i < output.size(); ++i)
        {
            const auto source = static_cast<double> (i) / ratio;
            const auto index = static_cast<size_t> (source);
            const auto fraction = static_cast<float> (source - static_cast<double> (index));

            const auto a = index < signal.size() ? signal[index] : 0.0f;
            const auto b = index + 1 < signal.size() ? signal[index + 1] : 0.0f;

            output[i] = a + (b - a) * fraction;
        }

        return output;
    }

    // How much finer than one sample the correlation is evaluated, as a power
    // of two. Zero-padding the cross-spectrum before the inverse transform is
    // exact sinc interpolation of the correlation, so this genuinely resolves
    // the peak rather than smoothing it.
    //
    // Why it is needed: fitting a parabola to the three samples straddling a
    // correlation peak is biased, worst when the true peak sits near a quarter
    // sample from the centre - measured 0.11 samples of error recovering a
    // 37.25-sample offset. Evaluating at quarter-sample resolution first cuts
    // that by an order of magnitude, comfortably inside the 0.1-sample
    // accuracy tests/IrAlignmentTests.cpp asserts.
    constexpr int correlationUpsampleOrder = 2;   // 4x

    // Cross-correlates two signals and returns the correlation evaluated at
    // 2^correlationUpsampleOrder points per sample. Index j corresponds to lag
    // j / 2^correlationUpsampleOrder; indices past the halfway point are
    // negative lags, as with any circular correlation.
    std::vector<float> correlateUpsampled (const std::vector<float>& a,
                                            const std::vector<float>& b,
                                            int fftOrder)
    {
        const auto fftSize = 1 << fftOrder;
        const auto numBins = fftSize / 2 + 1;

        juce::dsp::FFT forward (fftOrder);

        std::vector<float> specA (static_cast<size_t> (fftSize) * 2, 0.0f);
        std::vector<float> specB (static_cast<size_t> (fftSize) * 2, 0.0f);

        std::copy_n (a.begin(), std::min (a.size(), static_cast<size_t> (fftSize)), specA.begin());
        std::copy_n (b.begin(), std::min (b.size(), static_cast<size_t> (fftSize)), specB.begin());

        forward.performRealOnlyForwardTransform (specA.data(), true);
        forward.performRealOnlyForwardTransform (specB.data(), true);

        // Zero-padding the cross-spectrum into a longer transform interpolates
        // the correlation exactly (band-limited sinc interpolation), rather
        // than approximating it as a polynomial fit would.
        const auto upsampledOrder = fftOrder + correlationUpsampleOrder;
        const auto upsampledSize = 1 << upsampledOrder;

        juce::dsp::FFT inverse (upsampledOrder);

        std::vector<float> product (static_cast<size_t> (upsampledSize) * 2, 0.0f);

        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto ar = specA[static_cast<size_t> (bin) * 2];
            const auto ai = specA[static_cast<size_t> (bin) * 2 + 1];
            const auto br = specB[static_cast<size_t> (bin) * 2];
            const auto bi = specB[static_cast<size_t> (bin) * 2 + 1];

            product[static_cast<size_t> (bin) * 2] = ar * br + ai * bi;
            product[static_cast<size_t> (bin) * 2 + 1] = ai * br - ar * bi;
        }

        inverse.performRealOnlyInverseTransform (product.data());

        std::vector<float> result (static_cast<size_t> (upsampledSize), 0.0f);
        std::copy_n (product.begin(), upsampledSize, result.begin());

        return result;
    }

    // Lagrange-3 interpolation, matching FractionalDelay's kernel so an IR
    // shifted off-thread and a signal delayed on the audio thread are shaped
    // identically.
    float lagrange3 (float yMinus1, float y0, float y1, float y2, float t) noexcept
    {
        const auto c0 = y0;
        const auto c1 = y1 - (1.0f / 3.0f) * yMinus1 - 0.5f * y0 - (1.0f / 6.0f) * y2;
        const auto c2 = 0.5f * (yMinus1 + y1) - y0;
        const auto c3 = (1.0f / 6.0f) * (y2 - yMinus1) + 0.5f * (y0 - y1);

        return ((c3 * t + c2) * t + c1) * t + c0;
    }
}

namespace IrAlignment
{
    int detectOnsetSample (const juce::AudioBuffer<float>& buffer, float thresholdRelativeToPeak) noexcept
    {
        const auto numChannels = buffer.getNumChannels();
        const auto numSamples = buffer.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return 0;

        float peak = 0.0f;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 0; sample < numSamples; ++sample)
                peak = juce::jmax (peak, std::abs (data[sample]));
        }

        // A silent/all-zero buffer has no meaningful onset; treat it as
        // starting at sample 0 (matches the default delta IR's own onset).
        if (peak <= std::numeric_limits<float>::epsilon())
            return 0;

        const auto threshold = peak * thresholdRelativeToPeak;

        for (int sample = 0; sample < numSamples; ++sample)
            for (int channel = 0; channel < numChannels; ++channel)
                if (std::abs (buffer.getSample (channel, sample)) >= threshold)
                    return sample;

        return 0;
    }

    juce::AudioBuffer<float> shiftBySamples (const juce::AudioBuffer<float>& buffer, int shiftSamples)
    {
        if (shiftSamples == 0)
            return buffer; // AudioBuffer's copy constructor deep-copies.

        const auto numChannels = buffer.getNumChannels();
        const auto numSamples = buffer.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return buffer;

        if (shiftSamples > 0)
        {
            // Delay: prepend `shiftSamples` zero samples ahead of the
            // existing content.
            juce::AudioBuffer<float> shifted (numChannels, numSamples + shiftSamples);
            shifted.clear();

            for (int channel = 0; channel < numChannels; ++channel)
                shifted.copyFrom (channel, shiftSamples, buffer, channel, 0, numSamples);

            return shifted;
        }

        // Advance: drop the leading -shiftSamples samples, permanently
        // discarding them - clamped so at least one sample always survives,
        // even for a pathological shift larger than the buffer itself.
        const auto samplesToDrop = juce::jmin (-shiftSamples, numSamples - 1);
        const auto remaining = numSamples - samplesToDrop;

        juce::AudioBuffer<float> shifted (numChannels, remaining);

        for (int channel = 0; channel < numChannels; ++channel)
            shifted.copyFrom (channel, 0, buffer, channel, samplesToDrop, remaining);

        return shifted;
    }

    juce::AudioBuffer<float> alignOnsetToReference (const juce::AudioBuffer<float>& target,
                                                     double targetSampleRate,
                                                     int referenceOnsetSample,
                                                     double referenceSampleRate)
    {
        if (targetSampleRate <= 0.0 || referenceSampleRate <= 0.0)
            return target;

        const auto targetOnsetSample = detectOnsetSample (target);

        const auto referenceOnsetSeconds = static_cast<double> (referenceOnsetSample) / referenceSampleRate;
        const auto targetOnsetSeconds = static_cast<double> (targetOnsetSample) / targetSampleRate;

        const auto shiftSeconds = referenceOnsetSeconds - targetOnsetSeconds;
        const auto shiftSamples = static_cast<int> (std::lround (shiftSeconds * targetSampleRate));

        return shiftBySamples (target, shiftSamples);
    }

    //==========================================================================
    Measurement measure (const juce::AudioBuffer<float>& reference,
                          double referenceSampleRate,
                          const juce::AudioBuffer<float>& target,
                          double targetSampleRate)
    {
        Measurement result;

        auto referenceMono = toMono (reference);
        auto targetMono = toMono (target);

        if (referenceMono.empty() || targetMono.empty()
            || referenceSampleRate <= 0.0 || targetSampleRate <= 0.0)
            return result;

        // Correlate on the target's time base, so the lag comes out directly
        // in the units the shift will be applied in.
        referenceMono = resampleForAnalysis (referenceMono, referenceSampleRate, targetSampleRate);

        if (referenceMono.empty())
            return result;

        const auto referenceEnergy = energyOf (referenceMono);
        const auto targetEnergy = energyOf (targetMono);

        // A silent buffer correlates with everything and nothing; there is no
        // lag to find.
        if (! (referenceEnergy > 0.0) || ! (targetEnergy > 0.0))
            return result;

        // Zero-padded to at least lenA + lenB - 1 so the circular correlation
        // does not wrap a real peak onto a false lag.
        const auto minimumSize = referenceMono.size() + targetMono.size();

        int fftOrder = 1;

        while ((static_cast<size_t> (1) << fftOrder) < minimumSize)
            ++fftOrder;

        fftOrder = juce::jmin (fftOrder, 18);

        const auto correlation = correlateUpsampled (targetMono, referenceMono, fftOrder);

        if (correlation.empty())
            return result;

        const auto fftSize = static_cast<int> (correlation.size());

        // Every index is now a fraction of a sample; convert once here so the
        // search below can stay in index space.
        constexpr int upsampleFactor = 1 << correlationUpsampleOrder;

        // Search both signs of lag: the target may arrive before or after the
        // reference. Negative lag -k lives at index fftSize - k.
        const auto searchLimit = juce::jmin (fftSize / 2,
                                              static_cast<int> (std::max (referenceMono.size(),
                                                                           targetMono.size()))
                                                  * upsampleFactor);

        int peakIndex = 0;
        float peakMagnitude = 0.0f;

        const auto considerIndex = [&] (int index)
        {
            const auto magnitude = std::abs (correlation[static_cast<size_t> (index)]);

            if (magnitude > peakMagnitude)
            {
                peakMagnitude = magnitude;
                peakIndex = index;
            }
        };

        for (int lag = 0; lag < searchLimit; ++lag)
        {
            considerIndex (lag);

            if (lag > 0)
                considerIndex (fftSize - lag);
        }

        // Normalised by the geometric mean of the two energies (Cauchy-Schwarz),
        // so 1.0 means the IRs are scalar multiples and near-0 means they share
        // no structure worth aligning. The upsampled inverse transform carries
        // a 1/upsampleFactor scaling relative to the plain-size one, which is
        // undone here so the threshold means the same thing regardless of the
        // upsampling factor.
        result.normalisedPeak = static_cast<float> (peakMagnitude * upsampleFactor
                                                     / std::sqrt (referenceEnergy * targetEnergy));

        // The sign of the correlation peak is the polarity relationship: a
        // negative peak means the best match is achieved by flipping one of
        // them, i.e. summing as captured would partially cancel.
        result.polarityInverted = correlation[static_cast<size_t> (peakIndex)] < 0.0f;

        // Parabolic refinement across the peak's two neighbours, on the
        // magnitude so the sign convention above stays independent of it.
        const auto neighbour = [&] (int offset)
        {
            auto index = (peakIndex + offset) % fftSize;

            while (index < 0)
                index += fftSize;

            return std::abs (correlation[static_cast<size_t> (index)]);
        };

        const auto refinement = MinPhase::parabolicPeakOffset (neighbour (-1),
                                                                std::abs (correlation[static_cast<size_t> (peakIndex)]),
                                                                neighbour (1));

        // Unwrap: indices above half the FFT size are negative lags. The result
        // is in upsampled steps, so scale back to samples.
        const auto signedPeak = peakIndex > fftSize / 2 ? peakIndex - fftSize : peakIndex;

        result.lagSamples = (static_cast<float> (signedPeak) + refinement)
                             / static_cast<float> (upsampleFactor);

        return result;
    }

    juce::AudioBuffer<float> invertPolarity (const juce::AudioBuffer<float>& buffer)
    {
        juce::AudioBuffer<float> result;
        result.makeCopyOf (buffer);
        result.applyGain (-1.0f);

        return result;
    }

    juce::AudioBuffer<float> shiftByFractionalSamples (const juce::AudioBuffer<float>& buffer,
                                                        float shiftSamples)
    {
        const auto integerShift = static_cast<int> (std::floor (shiftSamples));
        const auto fraction = shiftSamples - static_cast<float> (integerShift);

        // The integer part first, by whole-sample moves - exact, and it means a
        // whole-sample shift never touches the interpolator at all.
        auto shifted = shiftBySamples (buffer, integerShift);

        if (fraction < 1.0e-6f)
            return shifted;

        const auto numChannels = shifted.getNumChannels();
        const auto numSamples = shifted.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return shifted;

        juce::AudioBuffer<float> result (numChannels, numSamples);
        result.clear();

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto* source = shifted.getReadPointer (channel);
            auto* destination = result.getWritePointer (channel);

            const auto sampleAt = [&] (int index) -> float
            {
                return (index >= 0 && index < numSamples) ? source[index] : 0.0f;
            };

            // Reading one sample *behind* the output index delays by the
            // fractional amount, matching shiftBySamples()'s sign convention
            // (positive shift = later).
            for (int i = 0; i < numSamples; ++i)
                destination[i] = lagrange3 (sampleAt (i + 1), sampleAt (i),
                                             sampleAt (i - 1), sampleAt (i - 2),
                                             fraction);
        }

        return result;
    }

    juce::AudioBuffer<float> alignToReference (const juce::AudioBuffer<float>& target,
                                                double targetSampleRate,
                                                const juce::AudioBuffer<float>& referenceBuffer,
                                                int referenceOnsetSample,
                                                double referenceSampleRate,
                                                Mode mode)
    {
        const auto legacy = [&]
        {
            return alignOnsetToReference (target, targetSampleRate,
                                           referenceOnsetSample, referenceSampleRate);
        };

        if (mode == Mode::Legacy)
            return legacy();

        if (referenceBuffer.getNumSamples() <= 0 || referenceBuffer.getNumChannels() <= 0)
            return legacy();

        const auto measurement = measure (referenceBuffer, referenceSampleRate,
                                           target, targetSampleRate);

        // Too weak a correlation means the two IRs have little in common - a
        // cabinet and a plate reverb, say. The measured lag would then be
        // whichever noise peak happened to win, so fall back to the heuristic
        // rather than shifting by a number that means nothing.
        if (measurement.normalisedPeak < minimumTrustedCorrelation)
            return legacy();

        auto aligned = measurement.polarityInverted ? invertPolarity (target) : target;

        // A positive lag means the target arrives late, so it is advanced by
        // shifting it earlier - hence the negation.
        return shiftByFractionalSamples (aligned, -measurement.lagSamples);
    }
}
