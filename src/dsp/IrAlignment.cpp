#include "IrAlignment.h"

#include <cmath>
#include <limits>

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
}
