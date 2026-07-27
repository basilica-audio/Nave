#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <vector>

// A multi-channel fractional-sample delay line with a one-pole-smoothed delay
// time, used by three v0.3.0 features that all need "delay this path by a
// continuously variable, sub-sample amount without clicking":
//
//   * the Morph engine's interpolated bulk delay (tau), applied after the
//     morph convolver (src/dsp/MorphEngine.h)
//   * the IR B Delay control's dual-sided branch delays
//   * Distance Air's time-of-flight wet pre-delay
//
// Interpolation is 3rd-order Lagrange (cubic) over four taps. Cubic is the
// right trade here: linear interpolation's magnitude response droops audibly
// toward Nyquist (it is a 2-tap lowpass whose cutoff moves with the fractional
// part, so automating the delay would sound like automating a tone control),
// while an allpass interpolator's transient response rings when the delay is
// modulated - exactly the case these three features are built for.
//
// The delay *time* is smoothed by a one-pole rather than snapped, so
// automating any of the three controls glides the read pointer instead of
// jumping it. That glide is the physically correct behaviour: a mic that
// changes distance genuinely Doppler-shifts, and the alternative (an
// instantaneous jump) is a click.
//
// prepare() allocates; process()/processChannel() do not, and never resize
// anything, so they are safe on the audio thread.
class FractionalDelay
{
public:
    FractionalDelay() = default;

    // Allocates the delay buffers. `maximumDelaySamples` is the largest delay
    // time that can subsequently be requested; requests beyond it are clamped.
    // Not real-time safe.
    void prepare (double newSampleRate, int numChannels, int maximumDelaySamples)
    {
        sampleRate = newSampleRate;

        // +4 taps of headroom for the cubic interpolator's window, and a
        // minimum size so a zero-delay configuration still has a valid buffer.
        bufferLength = juce::jmax (8, maximumDelaySamples + 4);

        buffers.assign (static_cast<size_t> (juce::jmax (1, numChannels)),
                         std::vector<float> (static_cast<size_t> (bufferLength), 0.0f));

        writeIndex = 0;
        maximumDelay = static_cast<float> (maximumDelaySamples);

        setSmoothingTimeSeconds (smoothingTimeSeconds);
        reset();
    }

    // Clears the delay line's history and snaps the smoothed delay time to its
    // target, so the next processed sample starts from a clean state. Safe on
    // the audio thread.
    void reset() noexcept
    {
        for (auto& buffer : buffers)
            std::fill (buffer.begin(), buffer.end(), 0.0f);

        writeIndex = 0;
        currentDelay = targetDelay;
    }

    // The one-pole time constant for delay-time changes. 50 ms is the value
    // the v0.3.0 brief specifies for all three call sites: long enough that a
    // fast knob sweep glides rather than steps, short enough that the delay
    // still tracks a deliberate move.
    void setSmoothingTimeSeconds (double seconds) noexcept
    {
        smoothingTimeSeconds = seconds;

        smoothingCoefficient = (sampleRate > 0.0 && seconds > 0.0)
                                    ? static_cast<float> (std::exp (-1.0 / (seconds * sampleRate)))
                                    : 0.0f;
    }

    // Requests a new delay in samples (may be fractional). The change is
    // approached exponentially, not applied immediately. Safe on the audio
    // thread.
    void setDelaySamples (float newDelaySamples) noexcept
    {
        targetDelay = juce::jlimit (0.0f, maximumDelay, newDelaySamples);
    }

    // Snaps both the target and the current delay, bypassing the smoothing.
    // Used when (re)configuring rather than automating - e.g. priming the line
    // in prepare(), where a glide from zero would be an artefact, not a
    // feature.
    void setDelaySamplesImmediate (float newDelaySamples) noexcept
    {
        setDelaySamples (newDelaySamples);
        currentDelay = targetDelay;
    }

    float getCurrentDelaySamples() const noexcept { return currentDelay; }
    float getTargetDelaySamples() const noexcept { return targetDelay; }

    int getNumChannels() const noexcept { return static_cast<int> (buffers.size()); }

    // Processes `block` in place. Every channel advances through the same
    // smoothed delay-time trajectory, so a stereo signal stays phase-coherent
    // across the two sides while the delay moves. No allocation.
    void process (juce::dsp::AudioBlock<float>& block) noexcept
    {
        const auto numSamples = static_cast<int> (block.getNumSamples());
        const auto numChannels = juce::jmin (static_cast<int> (block.getNumChannels()),
                                              static_cast<int> (buffers.size()));

        if (numSamples <= 0 || numChannels <= 0)
            return;

        for (int sample = 0; sample < numSamples; ++sample)
        {
            // The smoother advances once per *sample frame*, not once per
            // channel-sample, so both channels read the same delay time.
            currentDelay = targetDelay + (currentDelay - targetDelay) * smoothingCoefficient;

            for (int channel = 0; channel < numChannels; ++channel)
            {
                auto* data = block.getChannelPointer (static_cast<size_t> (channel));
                data[sample] = processSampleInChannel (channel, data[sample], currentDelay);
            }

            advanceWriteIndex();
        }
    }

private:
    void advanceWriteIndex() noexcept
    {
        writeIndex = (writeIndex + 1) % bufferLength;
    }

    // Writes `input` at the current write position and reads back
    // `delaySamples` earlier, cubically interpolated. The caller is
    // responsible for advancing the write index once all channels have been
    // processed for this sample frame.
    float processSampleInChannel (int channel, float input, float delaySamples) noexcept
    {
        auto& buffer = buffers[static_cast<size_t> (channel)];

        buffer[static_cast<size_t> (writeIndex)] = input;

        // Lagrange-3 needs one sample either side of the two straddling the
        // read position, so the usable delay range starts at 1 sample. Below
        // that the interpolator would need a *future* sample; clamping to 1
        // and letting the fractional part do the work keeps the mapping
        // continuous through zero at the cost of a fixed 1-sample offset,
        // which is why callers that must be sample-exact at zero delay skip
        // this processor entirely instead of asking it for zero.
        const auto clamped = juce::jlimit (1.0f, maximumDelay > 1.0f ? maximumDelay : 1.0f, delaySamples);

        const auto integerDelay = static_cast<int> (std::floor (clamped));
        const auto fraction = clamped - static_cast<float> (integerDelay);

        // Read four consecutive taps ending at the integer delay, oldest
        // first, wrapping around the circular buffer.
        const auto tapAt = [&] (int offset) -> float
        {
            auto index = writeIndex - integerDelay + 1 - offset;

            while (index < 0)
                index += bufferLength;

            return buffer[static_cast<size_t> (index % bufferLength)];
        };

        const auto yMinus1 = tapAt (-1);
        const auto y0 = tapAt (0);
        const auto y1 = tapAt (1);
        const auto y2 = tapAt (2);

        return lagrange3 (yMinus1, y0, y1, y2, fraction);
    }

    // 3rd-order Lagrange interpolation between y0 and y1, with y[-1]/y[2] as
    // the outer window taps, at fractional position `t` in [0, 1].
    static float lagrange3 (float yMinus1, float y0, float y1, float y2, float t) noexcept
    {
        const auto c0 = y0;
        const auto c1 = y1 - (1.0f / 3.0f) * yMinus1 - 0.5f * y0 - (1.0f / 6.0f) * y2;
        const auto c2 = 0.5f * (yMinus1 + y1) - y0;
        const auto c3 = (1.0f / 6.0f) * (y2 - yMinus1) + 0.5f * (y0 - y1);

        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    double sampleRate = 44100.0;
    double smoothingTimeSeconds = 0.05;

    std::vector<std::vector<float>> buffers;
    int bufferLength = 8;
    int writeIndex = 0;

    float maximumDelay = 0.0f;
    float targetDelay = 0.0f;
    float currentDelay = 0.0f;
    float smoothingCoefficient = 0.0f;
};
