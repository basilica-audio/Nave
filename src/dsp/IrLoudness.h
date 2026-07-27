#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Loudness-matched IR normalisation.
//
// JUCE's Convolution::Normalise::yes scales an IR to a fixed *energy*
// reference - a flat-weighted integral of the squared samples. That makes two
// IRs measure the same but not sound the same: a dark 4x12 capture carries far
// more of its energy below 200 Hz, where the ear is least sensitive, than a
// bright close-mic capture does, so energy-matching leaves the dark one
// audibly quieter. A/B-ing two IRs then compares level as much as tone, which
// is the wrong comparison to hand a user.
//
// Loudness mode instead weights the IR through the ITU-R BS.1770 K-weighting
// pre-filter (the same two-stage filter every LUFS meter uses) before
// integrating, so the reference is perceptual. Switching IRs under Loudness
// mode changes tone without lurching in level.
//
// The K-weighting filter is two biquads:
//   stage 1: a +4 dB high-shelf at ~1681.97 Hz (a rough head/torso model)
//   stage 2: an RLB high-pass at ~38.13 Hz (discards inaudible sub-bass)
// The published coefficients are specified at 48 kHz; for other rates they are
// re-derived here from the same analog prototypes so the weighting curve stays
// put rather than scaling with sample rate.
//
// None of this is real-time safe (it allocates and runs over whole IR
// buffers); callers must only invoke it off the audio thread, the same
// contract as MinPhase and IrAlignment.
namespace IrLoudness
{
    // The reference amplitude both gain modes target: -18 dBFS, matching the
    // energy reference JUCE's own Normalise::yes uses, so Energy and Loudness
    // mode land in the same ballpark rather than one being systematically
    // louder than the other.
    inline constexpr float referenceAmplitude = 0.125f;

    // Biquad coefficients in the {b0, b1, b2, a1, a2} form used below (a0
    // normalised to 1).
    struct BiquadCoefficients
    {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
    };

    // BS.1770 stage 1: the high-frequency shelving filter.
    BiquadCoefficients makeStage1ShelfCoefficients (double sampleRate) noexcept;

    // BS.1770 stage 2: the RLB high-pass.
    BiquadCoefficients makeStage2HighPassCoefficients (double sampleRate) noexcept;

    // Filters a copy of `buffer` through both K-weighting stages and returns
    // the total energy (sum of squares across all channels) of the result.
    double computeKWeightedEnergy (const juce::AudioBuffer<float>& buffer, double sampleRate);

    // Unweighted sum of squares across all channels - the quantity JUCE's own
    // energy normalisation works from, reimplemented here so both gain modes
    // can be computed by the same code path.
    double computeEnergy (const juce::AudioBuffer<float>& buffer) noexcept;

    // The gain that scales `buffer` to referenceAmplitude on a K-weighted
    // energy basis. Returns 1.0 for a silent or empty buffer (nothing
    // meaningful to normalise, and any other answer would be a division by
    // zero dressed up as a number).
    float computeLoudnessGain (const juce::AudioBuffer<float>& buffer, double sampleRate);

    // The same, on an unweighted energy basis. Provided for symmetry and for
    // the tests that compare the two weightings against each other; the
    // production Energy path leaves normalisation to JUCE.
    float computeEnergyGain (const juce::AudioBuffer<float>& buffer) noexcept;

    // Returns a copy of `buffer` scaled by computeLoudnessGain().
    juce::AudioBuffer<float> applyLoudnessNormalisation (const juce::AudioBuffer<float>& buffer,
                                                          double sampleRate);
}
