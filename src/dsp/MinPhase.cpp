#include "MinPhase.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace
{
    // juce::dsp::FFT's performRealOnlyForwardTransform() wants a buffer of
    // 2 * fftSize floats and writes interleaved {re, im} pairs for bins
    // 0..fftSize/2 (JUCE 8.0.14, juce_FFT.h). Everything below follows that
    // layout rather than the packed-Nyquist convention some other libraries
    // use.
    int binCountForOrder (int fftOrder) noexcept
    {
        return (1 << fftOrder) / 2 + 1;
    }

    // A half-Hann (raised-cosine) fade applied to the final `fadeLength`
    // samples of `data`, so truncating an IR (or a resynthesised morph
    // impulse) to a fixed length doesn't leave a step discontinuity that would
    // ring across the whole spectrum.
    void applyTailFade (float* data, int length, int fadeLength) noexcept
    {
        fadeLength = juce::jmin (fadeLength, length);

        if (fadeLength <= 1)
            return;

        const auto start = length - fadeLength;

        for (int i = 0; i < fadeLength; ++i)
        {
            const auto phase = juce::MathConstants<float>::pi
                                * static_cast<float> (i) / static_cast<float> (fadeLength - 1);
            data[start + i] *= 0.5f * (1.0f + std::cos (phase));
        }
    }
}

namespace MinPhase
{
    int chooseAnalysisOrder (int irLength) noexcept
    {
        // 4x the IR length, not 2x. The cepstrum of a real IR is not strictly
        // time-limited, so it aliases around the analysis window; at 2x
        // oversampling that aliasing is large enough to visibly distort the
        // resynthesised magnitude response (measured: ~16 dB worst-bin error
        // on a 4096-tap measured-style IR, against ~0.05 dB at 4x). 4x is the
        // smallest factor that holds the +/-0.1 dB per-bin accuracy
        // tests/MinPhaseTests.cpp asserts.
        const auto target = juce::jlimit (2, maxAnalysisSize, irLength * 4);

        int order = 1;

        while ((1 << order) < target)
            ++order;

        // Never below 8 (256 taps): the cepstral fold needs enough frequency
        // resolution that the log-spectrum is smooth, and a tiny FFT would
        // alias the transform's own time-domain spread back onto itself.
        return juce::jlimit (8, 16, order);
    }

    std::vector<float> prepareForAnalysis (const juce::AudioBuffer<float>& buffer)
    {
        const auto numChannels = buffer.getNumChannels();
        const auto numSamples = buffer.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return {};

        const auto length = juce::jmin (numSamples, maxIrLengthForAnalysis);

        std::vector<float> mono (static_cast<size_t> (length), 0.0f);

        const auto scale = 1.0f / static_cast<float> (numChannels);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto* source = buffer.getReadPointer (channel);

            for (int i = 0; i < length; ++i)
                mono[static_cast<size_t> (i)] += source[i] * scale;
        }

        // Only fade when content was actually cut off - an IR that already
        // fits must come through completely untouched, or a min-phase
        // round-trip could never be bit-exact.
        if (numSamples > maxIrLengthForAnalysis)
            applyTailFade (mono.data(), length, length / 4);

        return mono;
    }

    void computeLogMagnitude (const std::vector<float>& impulse,
                              int fftOrder,
                              std::vector<float>& out)
    {
        juce::dsp::FFT fft (fftOrder);

        const auto fftSize = 1 << fftOrder;
        const auto numBins = binCountForOrder (fftOrder);

        std::vector<float> scratch (static_cast<size_t> (fftSize) * 2, 0.0f);

        const auto copyLength = juce::jmin (static_cast<int> (impulse.size()), fftSize);
        std::copy_n (impulse.begin(), copyLength, scratch.begin());

        fft.performRealOnlyForwardTransform (scratch.data(), true);

        out.assign (static_cast<size_t> (numBins), 0.0f);

        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto re = scratch[static_cast<size_t> (bin) * 2];
            const auto im = scratch[static_cast<size_t> (bin) * 2 + 1];
            const auto magnitude = std::sqrt (re * re + im * im);

            out[static_cast<size_t> (bin)] = std::log (juce::jmax (magnitude, magnitudeFloor));
        }
    }

    void resynthesiseFromLogMagnitude (const std::vector<float>& logMagnitude,
                                        int fftOrder,
                                        int outputLength,
                                        std::vector<float>& out)
    {
        juce::dsp::FFT fft (fftOrder);

        const auto fftSize = 1 << fftOrder;
        const auto numBins = binCountForOrder (fftOrder);

        out.assign (static_cast<size_t> (juce::jmax (1, outputLength)), 0.0f);

        if (static_cast<int> (logMagnitude.size()) < numBins)
            return;

        // Step 1: the real cepstrum. The log-magnitude spectrum is real and
        // even, so its inverse transform is real and even too - a real-only
        // inverse FFT is exactly right here.
        std::vector<float> cepstrum (static_cast<size_t> (fftSize) * 2, 0.0f);

        for (int bin = 0; bin < numBins; ++bin)
        {
            cepstrum[static_cast<size_t> (bin) * 2] = logMagnitude[static_cast<size_t> (bin)];
            cepstrum[static_cast<size_t> (bin) * 2 + 1] = 0.0f;
        }

        fft.performRealOnlyInverseTransform (cepstrum.data());

        // Step 2: the fold. A minimum-phase signal is exactly one whose
        // cepstrum is causal, so doubling the causal half and zeroing the
        // anti-causal half projects onto the minimum-phase subspace. DC and
        // Nyquist are shared between the two halves and so are left alone.
        std::vector<float> folded (static_cast<size_t> (fftSize) * 2, 0.0f);

        folded[0] = cepstrum[0];

        for (int n = 1; n < fftSize / 2; ++n)
            folded[static_cast<size_t> (n)] = 2.0f * cepstrum[static_cast<size_t> (n)];

        folded[static_cast<size_t> (fftSize / 2)] = cepstrum[static_cast<size_t> (fftSize / 2)];

        // Step 3: exponentiate back into the spectral domain. exp() of a
        // complex number needs the full complex transform, so this stage is
        // done by hand rather than with the real-only helpers.
        fft.performRealOnlyForwardTransform (folded.data(), true);

        std::vector<float> spectrum (static_cast<size_t> (fftSize) * 2, 0.0f);

        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto re = folded[static_cast<size_t> (bin) * 2];
            const auto im = folded[static_cast<size_t> (bin) * 2 + 1];

            const auto magnitude = std::exp (re);

            spectrum[static_cast<size_t> (bin) * 2] = magnitude * std::cos (im);
            spectrum[static_cast<size_t> (bin) * 2 + 1] = magnitude * std::sin (im);
        }

        fft.performRealOnlyInverseTransform (spectrum.data());

        const auto copyLength = juce::jmin (outputLength, fftSize);
        std::copy_n (spectrum.begin(), copyLength, out.begin());

        // Only fade when the resynthesised impulse is genuinely longer than
        // the requested output, i.e. when something is being cut off.
        if (fftSize > outputLength)
            applyTailFade (out.data(), copyLength, copyLength / 4);
    }

    juce::AudioBuffer<float> transform (const juce::AudioBuffer<float>& buffer)
    {
        const auto numChannels = buffer.getNumChannels();
        const auto numSamples = buffer.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0)
            return buffer;

        const auto analysis = prepareForAnalysis (buffer);

        if (analysis.empty())
            return buffer;

        const auto fftOrder = chooseAnalysisOrder (static_cast<int> (analysis.size()));

        std::vector<float> logMagnitude;
        computeLogMagnitude (analysis, fftOrder, logMagnitude);

        std::vector<float> minimumPhase;
        resynthesiseFromLogMagnitude (logMagnitude, fftOrder, numSamples, minimumPhase);

        juce::AudioBuffer<float> result (numChannels, numSamples);

        // Every channel gets the same minimum-phase impulse, scaled to
        // preserve that channel's own energy relative to the mono analysis.
        // Rebuilding each channel from an independent analysis instead would
        // destroy the inter-channel phase relationship a true-stereo IR
        // carries.
        const auto analysisEnergy = std::inner_product (analysis.begin(), analysis.end(),
                                                         analysis.begin(), 0.0);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto* source = buffer.getReadPointer (channel);

            double channelEnergy = 0.0;

            for (int i = 0; i < numSamples; ++i)
                channelEnergy += static_cast<double> (source[i]) * source[i];

            const auto gain = analysisEnergy > 0.0
                                   ? static_cast<float> (std::sqrt (channelEnergy / analysisEnergy))
                                   : 1.0f;

            auto* destination = result.getWritePointer (channel);

            for (int i = 0; i < numSamples; ++i)
                destination[i] = minimumPhase[static_cast<size_t> (i)] * gain;
        }

        return result;
    }

    float parabolicPeakOffset (float left, float centre, float right) noexcept
    {
        const auto denominator = left - 2.0f * centre + right;

        // A zero denominator means the three points are collinear (no peak to
        // refine); a positive one means the vertex is a minimum, not a
        // maximum - both are signs the "peak" is flat or noise, so decline to
        // refine rather than returning a wild extrapolation.
        if (! (denominator < 0.0f))
            return 0.0f;

        const auto offset = 0.5f * (left - right) / denominator;

        return juce::jlimit (-0.5f, 0.5f, offset);
    }

    void crossCorrelate (const std::vector<float>& a,
                          const std::vector<float>& b,
                          int fftOrder,
                          std::vector<float>& out)
    {
        juce::dsp::FFT fft (fftOrder);

        const auto fftSize = 1 << fftOrder;
        const auto numBins = binCountForOrder (fftOrder);

        std::vector<float> specA (static_cast<size_t> (fftSize) * 2, 0.0f);
        std::vector<float> specB (static_cast<size_t> (fftSize) * 2, 0.0f);

        std::copy_n (a.begin(), juce::jmin (static_cast<int> (a.size()), fftSize), specA.begin());
        std::copy_n (b.begin(), juce::jmin (static_cast<int> (b.size()), fftSize), specB.begin());

        fft.performRealOnlyForwardTransform (specA.data(), true);
        fft.performRealOnlyForwardTransform (specB.data(), true);

        // Correlation is convolution with one input time-reversed, which in
        // the frequency domain is multiplication by the conjugate.
        std::vector<float> product (static_cast<size_t> (fftSize) * 2, 0.0f);

        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto ar = specA[static_cast<size_t> (bin) * 2];
            const auto ai = specA[static_cast<size_t> (bin) * 2 + 1];
            const auto br = specB[static_cast<size_t> (bin) * 2];
            const auto bi = specB[static_cast<size_t> (bin) * 2 + 1];

            product[static_cast<size_t> (bin) * 2] = ar * br + ai * bi;
            product[static_cast<size_t> (bin) * 2 + 1] = ai * br - ar * bi;
        }

        fft.performRealOnlyInverseTransform (product.data());

        out.assign (static_cast<size_t> (fftSize), 0.0f);
        std::copy_n (product.begin(), fftSize, out.begin());
    }

    float estimateBulkDelaySamples (const juce::AudioBuffer<float>& buffer)
    {
        const auto original = prepareForAnalysis (buffer);

        if (original.empty())
            return 0.0f;

        const auto fftOrder = chooseAnalysisOrder (static_cast<int> (original.size()));

        std::vector<float> logMagnitude;
        computeLogMagnitude (original, fftOrder, logMagnitude);

        std::vector<float> minimumPhase;
        resynthesiseFromLogMagnitude (logMagnitude, fftOrder,
                                       static_cast<int> (original.size()), minimumPhase);

        // The correlation is searched over non-negative lags only: the
        // minimum-phase version is by definition the least-delayed signal with
        // this magnitude response, so the original can only ever sit at or
        // behind it.
        std::vector<float> correlation;
        crossCorrelate (original, minimumPhase, fftOrder, correlation);

        const auto searchLimit = juce::jmin (static_cast<int> (correlation.size()) / 2,
                                              static_cast<int> (original.size()));

        int peakIndex = 0;
        float peakValue = 0.0f;

        for (int lag = 0; lag < searchLimit; ++lag)
        {
            const auto value = correlation[static_cast<size_t> (lag)];

            if (value > peakValue)
            {
                peakValue = value;
                peakIndex = lag;
            }
        }

        if (peakIndex <= 0 || peakIndex + 1 >= searchLimit)
            return static_cast<float> (peakIndex);

        const auto offset = parabolicPeakOffset (correlation[static_cast<size_t> (peakIndex - 1)],
                                                  correlation[static_cast<size_t> (peakIndex)],
                                                  correlation[static_cast<size_t> (peakIndex + 1)]);

        return static_cast<float> (peakIndex) + offset;
    }
}
