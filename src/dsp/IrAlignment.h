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
    // How IR B is aligned against IR A.
    //
    // Legacy is exactly what v0.2 shipped: find each buffer's first sample
    // above 20% of its own peak, and shift by the whole-sample difference. It
    // is kept bit-exact (not merely "similar") because the v1 -> v2 state
    // migration writes it into every upgraded session, so a v0.2 project must
    // keep rendering identically.
    //
    // Precise replaces the heuristic with a measurement: FFT cross-correlation
    // between the two IRs, parabolic refinement of the peak to sub-sample
    // resolution, fractional-delay application of the remainder, and automatic
    // polarity detection from the sign of the correlation peak. The onset
    // heuristic fails exactly where it matters most - two captures of the same
    // cabinet whose transients have different rise shapes cross a relative
    // threshold at different points, so the "aligned" result still combs.
    // Cross-correlation aligns the whole waveform rather than one point on it.
    enum class Mode
    {
        Legacy,
        Precise
    };

    // The result of a Precise-mode measurement.
    struct Measurement
    {
        // How far `target` lags `reference`, in (fractional) samples. Positive
        // means the target arrives later and must be advanced.
        float lagSamples = 0.0f;

        // True when the two IRs correlate best with opposite polarity, i.e.
        // summing them as captured would subtract rather than add. Inverting
        // one is then the difference between a full-bodied blend and a hollow
        // one.
        bool polarityInverted = false;

        // Peak correlation magnitude, normalised so 1.0 means the two IRs are
        // scalar multiples of each other. Near zero means the IRs share no
        // structure and the lag estimate is not meaningful.
        float normalisedPeak = 0.0f;
    };

    // Cross-correlates `target` against `reference` (both summed to mono) and
    // returns the sub-sample lag, polarity and correlation strength. Both
    // buffers are interpreted at their own sample rate; the returned lag is in
    // samples of `targetSampleRate`. Off the audio thread only.
    Measurement measure (const juce::AudioBuffer<float>& reference,
                          double referenceSampleRate,
                          const juce::AudioBuffer<float>& target,
                          double targetSampleRate);

    // Shifts `buffer` earlier or later by a fractional number of samples,
    // using the same Lagrange-3 kernel the audio-thread delay lines use. The
    // integer part is applied by shifting whole samples (exact) and only the
    // remainder goes through the interpolator, so a whole-sample shift is
    // bit-exact rather than merely close.
    juce::AudioBuffer<float> shiftByFractionalSamples (const juce::AudioBuffer<float>& buffer,
                                                        float shiftSamples);

    // Returns a copy of `buffer` with every sample negated.
    juce::AudioBuffer<float> invertPolarity (const juce::AudioBuffer<float>& buffer);

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

    // The v0.3.0 entry point CabConvolutionEngine::setImpulseResponseB() uses.
    //
    // In Legacy mode this forwards to alignOnsetToReference() above, producing
    // bit-identical output to v0.2 - `referenceBuffer` is ignored and only the
    // recorded onset is used.
    //
    // In Precise mode it cross-correlates `target` against `referenceBuffer`,
    // applies the measured fractional lag, and inverts the target's polarity
    // when the correlation says the two would otherwise partially cancel.
    // Falls back to the Legacy path when the reference buffer is unavailable
    // or the two IRs correlate too weakly for the measurement to mean
    // anything - a guess dressed up as a measurement is worse than the
    // heuristic it replaced.
    //
    // Off the audio thread only.
    juce::AudioBuffer<float> alignToReference (const juce::AudioBuffer<float>& target,
                                                double targetSampleRate,
                                                const juce::AudioBuffer<float>& referenceBuffer,
                                                int referenceOnsetSample,
                                                double referenceSampleRate,
                                                Mode mode);

    // Below this normalised correlation peak, Precise mode declines to trust
    // its own measurement and falls back to the Legacy onset heuristic.
    inline constexpr float minimumTrustedCorrelation = 0.05f;
}
