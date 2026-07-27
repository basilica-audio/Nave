#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <vector>

// Cepstral minimum-phase machinery, shared by the Morph engine
// (src/dsp/MorphEngine.h) and the per-slot Min-Phase transform
// (irAMinPhase/irBMinPhase in CabConvolutionEngine).
//
// A minimum-phase filter has the same magnitude response as the original but
// the least possible phase lag, which front-loads its energy. Two IRs that
// have both been minimum-phased can be summed or interpolated without the
// comb-filtering their differing excess-phase components would otherwise
// cause - the reason every commercial cab simulator that morphs mic positions
// (Two Notes, Fractal's MPT) does this decomposition internally.
//
// The transform used here is the standard real-cepstrum fold:
//
//   c      = IFFT(log|FFT(h)|)                   (the real cepstrum)
//   c_min  = c with the anti-causal half folded onto the causal half
//   h_min  = IFFT(exp(FFT(c_min)))
//
// A minimum-phase signal's cepstrum is causal, so folding is exactly the
// projection onto the minimum-phase subspace.
//
// NONE of these functions are real-time safe: they allocate, run FFTs, and
// are O(N log N) in the analysis size. Callers must only invoke them off the
// audio thread - the same contract as IrAlignment and
// CabConvolutionEngine::setImpulseResponse().
namespace MinPhase
{
    // Magnitude floor applied before the log, i.e. -120 dB. Without it a bin
    // that happens to be exactly zero would produce log(0) = -inf and poison
    // the entire cepstrum.
    inline constexpr float magnitudeFloor = 1.0e-6f;

    // The largest analysis FFT the morph/MPT path will use. An IR longer than
    // half this is half-Hann-windowed down to maxAnalysisSize / 2 taps before
    // analysis (see chooseAnalysisOrder/prepareForAnalysis) - documented in
    // docs/manual.md, and inaudible for cabinet IRs, whose usable content is
    // over long before 16384 taps at any normal sample rate.
    inline constexpr int maxAnalysisSize = 32768;
    inline constexpr int maxIrLengthForAnalysis = maxAnalysisSize / 2;

    // The FFT order (log2 size) to analyse an IR of `irLength` taps with:
    // the next power of two at or above 2 * irLength, so the circular
    // convolution the cepstrum implies has room for the transform's own
    // time-domain spread, capped at maxAnalysisSize.
    int chooseAnalysisOrder (int irLength) noexcept;

    // Sums `buffer` to mono and, if it is longer than maxIrLengthForAnalysis,
    // truncates it to that length with a half-Hann fade over the final 25% so
    // the truncation itself doesn't ring. Returns a newly allocated vector.
    std::vector<float> prepareForAnalysis (const juce::AudioBuffer<float>& buffer);

    // log(max(|FFT(h)|, magnitudeFloor)) for the first (fftSize / 2 + 1) bins
    // of a real FFT of size 2^fftOrder. `out` is resized to that bin count.
    // This is the quantity the Morph engine interpolates linearly, which in
    // the linear domain is a geometric mean - the reason a 50/50 morph of two
    // IRs with a shared resonance keeps that resonance's level instead of
    // dipping, as an arithmetic (power) mean would.
    void computeLogMagnitude (const std::vector<float>& impulse,
                              int fftOrder,
                              std::vector<float>& out);

    // Resynthesises the minimum-phase impulse whose log-magnitude spectrum is
    // `logMagnitude` (as produced by computeLogMagnitude, i.e.
    // fftSize / 2 + 1 bins). `out` is resized to `outputLength` samples and
    // receives the leading `outputLength` taps of the result, with a half-Hann
    // fade over the final 25% to avoid a hard truncation edge.
    void resynthesiseFromLogMagnitude (const std::vector<float>& logMagnitude,
                                        int fftOrder,
                                        int outputLength,
                                        std::vector<float>& out);

    // The minimum-phase equivalent of `buffer`: same magnitude response, all
    // excess phase removed. Every channel is transformed against the same
    // (mono-summed) analysis, so a stereo IR keeps its inter-channel
    // relationship intact rather than having each side independently
    // re-phased. The returned buffer has the same channel count and length as
    // `buffer`.
    juce::AudioBuffer<float> transform (const juce::AudioBuffer<float>& buffer);

    // The bulk delay of `buffer` in (fractional) samples: how far its energy
    // sits behind that of its own minimum-phase equivalent. This is the "tau"
    // the Morph engine interpolates alongside the log-magnitude, and it is
    // what carries a mic's physical distance from the cone through the morph -
    // interpolating it produces the subtle Doppler glide of a moving mic
    // rather than a phase-cancelling jump. Estimated by cross-correlating the
    // buffer against its minimum-phase version and refining the peak
    // parabolically. Returns 0 for an empty or silent buffer.
    float estimateBulkDelaySamples (const juce::AudioBuffer<float>& buffer);

    // Parabolic sub-sample refinement of a discrete correlation peak: given
    // the three samples straddling the peak, returns the offset in [-0.5,
    // +0.5] of the true (interpolated) maximum from the centre sample.
    // Returns 0 when the three points are collinear or the curvature has the
    // wrong sign, which is what a flat or noise-dominated peak looks like.
    float parabolicPeakOffset (float left, float centre, float right) noexcept;

    // Full-length cross-correlation of `a` against `b` via FFT, evaluated at
    // every lag. `out` is resized to `fftSize` and holds the circular
    // correlation, so lag k for k <= fftSize / 2 is out[k] and negative lag -k
    // is out[fftSize - k]. Both inputs are zero-padded; the caller is
    // responsible for choosing an fftOrder large enough
    // (>= nextPow2(a.size() + b.size() - 1)) that the wrap-around does not
    // alias into the lag range being searched.
    void crossCorrelate (const std::vector<float>& a,
                          const std::vector<float>& b,
                          int fftOrder,
                          std::vector<float>& out);
}
