#include "IrLoudness.h"

#include <cmath>

namespace
{
    // Runs one biquad (Direct Form I) over a whole channel, starting from
    // rest. Off-thread only; the state is local so successive channels never
    // bleed into each other.
    void filterChannel (float* data, int numSamples, const IrLoudness::BiquadCoefficients& c) noexcept
    {
        double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

        for (int i = 0; i < numSamples; ++i)
        {
            const auto x0 = static_cast<double> (data[i]);
            const auto y0 = c.b0 * x0 + c.b1 * x1 + c.b2 * x2 - c.a1 * y1 - c.a2 * y2;

            x2 = x1;
            x1 = x0;
            y2 = y1;
            y1 = y0;

            data[i] = static_cast<float> (y0);
        }
    }
}

namespace IrLoudness
{
    BiquadCoefficients makeStage1ShelfCoefficients (double sampleRate) noexcept
    {
        // BS.1770's stage 1 is published as a coefficient table at 48 kHz.
        // Those numbers are the bilinear transform of a high-shelf whose
        // parameters are recovered here so the *same* analog curve is
        // realised at 44.1, 88.2, 96 kHz and so on. Re-using the 48 kHz
        // numbers directly at another rate would slide the shelf's corner
        // frequency in proportion to the rate change.
        constexpr double shelfFrequency = 1681.974450955533;
        constexpr double shelfGainDb = 3.999843853973347;
        constexpr double shelfQ = 0.7071752369554196;

        const auto amplitude = std::pow (10.0, shelfGainDb / 40.0);
        const auto omega = 2.0 * juce::MathConstants<double>::pi * shelfFrequency / sampleRate;
        const auto cosOmega = std::cos (omega);
        const auto sinOmega = std::sin (omega);
        const auto alpha = sinOmega / (2.0 * shelfQ);
        const auto sqrtAmplitudeAlpha = 2.0 * std::sqrt (amplitude) * alpha;

        const auto a0 = (amplitude + 1.0) - (amplitude - 1.0) * cosOmega + sqrtAmplitudeAlpha;

        BiquadCoefficients c;
        c.b0 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosOmega + sqrtAmplitudeAlpha) / a0;
        c.b1 = -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosOmega) / a0;
        c.b2 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosOmega - sqrtAmplitudeAlpha) / a0;
        c.a1 = 2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosOmega) / a0;
        c.a2 = ((amplitude + 1.0) - (amplitude - 1.0) * cosOmega - sqrtAmplitudeAlpha) / a0;

        return c;
    }

    BiquadCoefficients makeStage2HighPassCoefficients (double sampleRate) noexcept
    {
        // The RLB high-pass, likewise recovered from BS.1770's 48 kHz table
        // as an analog prototype so it survives a sample-rate change.
        constexpr double highPassFrequency = 38.13547087602444;
        constexpr double highPassQ = 0.5003270373238773;

        const auto omega = 2.0 * juce::MathConstants<double>::pi * highPassFrequency / sampleRate;
        const auto cosOmega = std::cos (omega);
        const auto sinOmega = std::sin (omega);
        const auto alpha = sinOmega / (2.0 * highPassQ);

        const auto a0 = 1.0 + alpha;

        BiquadCoefficients c;
        c.b0 = ((1.0 + cosOmega) * 0.5) / a0;
        c.b1 = (-(1.0 + cosOmega)) / a0;
        c.b2 = ((1.0 + cosOmega) * 0.5) / a0;
        c.a1 = (-2.0 * cosOmega) / a0;
        c.a2 = (1.0 - alpha) / a0;

        return c;
    }

    double computeEnergy (const juce::AudioBuffer<float>& buffer) noexcept
    {
        double energy = 0.0;

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int i = 0; i < buffer.getNumSamples(); ++i)
                energy += static_cast<double> (data[i]) * data[i];
        }

        return energy;
    }

    double computeKWeightedEnergy (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        if (buffer.getNumChannels() <= 0 || buffer.getNumSamples() <= 0 || sampleRate <= 0.0)
            return 0.0;

        juce::AudioBuffer<float> weighted;
        weighted.makeCopyOf (buffer);

        const auto stage1 = makeStage1ShelfCoefficients (sampleRate);
        const auto stage2 = makeStage2HighPassCoefficients (sampleRate);

        for (int channel = 0; channel < weighted.getNumChannels(); ++channel)
        {
            auto* data = weighted.getWritePointer (channel);
            filterChannel (data, weighted.getNumSamples(), stage1);
            filterChannel (data, weighted.getNumSamples(), stage2);
        }

        return computeEnergy (weighted);
    }

    float computeLoudnessGain (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        const auto energy = computeKWeightedEnergy (buffer, sampleRate);

        if (! (energy > 0.0))
            return 1.0f;

        return static_cast<float> (referenceAmplitude / std::sqrt (energy));
    }

    float computeEnergyGain (const juce::AudioBuffer<float>& buffer) noexcept
    {
        const auto energy = computeEnergy (buffer);

        if (! (energy > 0.0))
            return 1.0f;

        return static_cast<float> (referenceAmplitude / std::sqrt (energy));
    }

    juce::AudioBuffer<float> applyLoudnessNormalisation (const juce::AudioBuffer<float>& buffer,
                                                          double sampleRate)
    {
        juce::AudioBuffer<float> result;
        result.makeCopyOf (buffer);

        result.applyGain (computeLoudnessGain (buffer, sampleRate));

        return result;
    }
}
