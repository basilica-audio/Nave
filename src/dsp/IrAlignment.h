#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

// Small, pure (allocation-only-off-the-audio-thread) helper functions used to
// implement inter-IR phase alignment: when a second impulse response (IR B)
// is loaded to be blended against the primary one (IR A), its transient
// onset is shifted in time to line up with IR A's onset, so that blending
// the two convolution outputs doesn't introduce comb-filtering from a timing
// mismatch between them (two IRs with, say, a 3ms offset between their
// direct-sound arrivals will partially cancel across a wide band when
// crossfaded).
//
// None of these functions are real-time safe (they allocate) - callers must
// only invoke them off the audio thread, the same contract as
// CabConvolutionEngine::setImpulseResponse()/setImpulseResponseB().
namespace IrAlignment
{
    // Returns the index of the first sample (checked across all channels)
    // whose absolute value reaches `thresholdRelativeToPeak` of the buffer's
    // own peak absolute sample - a standard, simple onset-detection
    // heuristic (deliberately not a full cross-correlation search: cabinet
    // IRs have a single dominant direct-sound transient, so a relative-
    // threshold crossing on the buffer itself is sufficient and much
    // cheaper). Returns 0 for an empty, silent, or all-zero buffer.
    int detectOnsetSample (const juce::AudioBuffer<float>& buffer, float thresholdRelativeToPeak = 0.2f) noexcept;

    // Shifts `buffer` in time by `shiftSamples`, at the buffer's own sample
    // rate: a positive shift delays it (prepends `shiftSamples` zero
    // samples, growing the buffer), a negative shift advances it (drops the
    // leading `-shiftSamples` samples, shrinking the buffer - clamped so at
    // least one sample always remains). A zero shift returns an unmodified
    // copy. Always returns a newly allocated buffer; never mutates `buffer`.
    juce::AudioBuffer<float> shiftBySamples (const juce::AudioBuffer<float>& buffer, int shiftSamples);

    // Detects `target`'s onset and returns a copy of `target` shifted so
    // that onset lands at the same *time* (not raw sample index - the two
    // IRs may have been captured, and may be loaded, at different sample
    // rates) as `referenceOnsetSample` measured at `referenceSampleRate`.
    // This is the entry point CabConvolutionEngine::setImpulseResponseB()
    // uses to align a newly loaded IR B against the onset already recorded
    // for IR A.
    juce::AudioBuffer<float> alignOnsetToReference (const juce::AudioBuffer<float>& target,
                                                     double targetSampleRate,
                                                     int referenceOnsetSample,
                                                     double referenceSampleRate);
}
