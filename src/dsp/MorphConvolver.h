#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <vector>

// A uniformly-partitioned overlap-save convolver (UPOLS) whose impulse
// response can be exchanged mid-signal without a click.
//
// WHY NOT juce::dsp::Convolution: the stock engine has no crossfade hook.
// Calling loadImpulseResponse() while audio is running swaps the engine
// wholesale and resets its internal state, which is audible as a discontinuity
// - fine for the rare, deliberate act of loading a new IR file, unacceptable
// for the Morph blend path, which republishes a freshly interpolated IR every
// time the Blend knob moves (up to 30 times a second).
//
// THE SCHEME. One frequency-domain delay line (FDL) of input spectra is shared
// by TWO sets of filter spectra, "current" and "next":
//
//   Y_m(w) = sum_p X_{m-p}(w) . H_p(w)
//
// When the morph worker publishes a new IR, the audio thread runs both filter
// sets against that one shared input history and crossfades their two output
// signals over ~30 ms. Because the input history is shared and never reset,
// there is no state discontinuity at all - only two valid outputs of the same
// input, blended.
//
// THE CROSSFADE IS AMPLITUDE-COMPLEMENTARY (LINEAR), NOT EQUAL-POWER. This is
// deliberate and load-bearing. Equal-power (sin/cos) gains preserve *power*,
// which is the correct choice only for *uncorrelated* sources. Successive
// morph spectra are the opposite of uncorrelated - adjacent blend steps are
// nearly identical, and republishing an unchanged IR is perfectly correlated.
// Under sin/cos gains a perfectly-correlated exchange sums to sqrt(2) at the
// fade midpoint: a +3 dB bump on a swap that should be inaudible, and audible
// pumping when a Blend drag makes fades near-continuous. Linear complementary
// gains (g_next = t, g_current = 1 - t, summing to exactly 1 everywhere) make
// an identical-IR republish null exactly and a correlated swap level-flat.
//
// LATENCY IS ZERO. Each callback re-transforms the accumulated input segment
// rather than waiting for a partition to fill, so the first output sample is
// available in the same callback as the first input sample (the scheme JUCE's
// own zero-latency Convolution configuration uses). getLatencySamples()
// returns 0 in every configuration.
//
// THREADING. prepare() allocates everything; process() allocates nothing and
// takes no locks. Publication is a lock-free handoff: the worker fills the
// pending spectra slot and flips an atomic flag, the audio thread picks it up
// at the next block boundary. Only one publisher at a time is supported, which
// is what MorphEngine's single worker thread provides.
class MorphConvolver
{
public:
    MorphConvolver() = default;

    // The longest IR (in taps) the convolver will accept. Matches the morph
    // analysis cap in MinPhase.h, so an IR that survived morph analysis always
    // fits here.
    static constexpr int maxImpulseLength = 16384;

    // The crossfade length for an IR exchange, in seconds. Long enough that no
    // exchange steps, short enough that a fast Blend drag's fades still finish
    // before the next publication supersedes them.
    static constexpr double crossfadeSeconds = 0.03;

    // Allocates all spectra, FFT scratch and output buffers. Not real-time
    // safe. Any previously published IR is discarded; the caller republishes
    // after re-preparing.
    void prepare (const juce::dsp::ProcessSpec& spec);

    // Clears the input history, both output accumulators and any in-flight
    // crossfade, leaving the currently loaded IR active. Safe on the audio
    // thread.
    void reset() noexcept;

    // Processes `block` in place. No allocation, no locks. A block larger than
    // the maximumBlockSize declared to prepare() is processed in
    // maximumBlockSize-sized chunks rather than overrunning anything.
    void process (juce::dsp::AudioBlock<float>& block) noexcept;

    // Publishes a new impulse response. Called from the morph worker thread
    // (or synchronously from tests). The IR is transformed into partition
    // spectra in the caller's thread - this allocates nothing, but it is
    // O(P log K) work and must not run on the audio thread.
    //
    // If a previous publication has not yet been consumed, it is overwritten:
    // latest-wins, so a fast Blend drag never queues a backlog of stale
    // intermediate IRs.
    //
    // Returns false if the convolver has not been prepared or the IR is
    // empty/too long.
    bool publishImpulseResponse (const float* impulse, int numSamples);

    // True while a published IR is waiting to be picked up by the audio
    // thread, or while its crossfade is still running. Tests use this to wait
    // for an exchange to complete deterministically.
    bool isExchangeInProgress() const noexcept
    {
        return pendingAvailable.load (std::memory_order_acquire) || crossfadeSamplesRemaining > 0;
    }

    // Always zero - see the class docs' latency note. Present so callers can
    // treat this interchangeably with juce::dsp::Convolution.
    int getLatencySamples() const noexcept { return 0; }

    // The partition size (B0) chosen by prepare(), exposed for tests.
    int getPartitionSize() const noexcept { return partitionSize; }

    // The number of partitions the currently active IR occupies.
    int getNumActivePartitions() const noexcept { return numActivePartitions; }

private:
    // A set of filter spectra: one interleaved {re, im} spectrum per
    // partition, plus how many partitions are actually in use.
    struct FilterSpectra
    {
        std::vector<float> data;   // numPartitions * fftSize * 2 floats
        int numPartitions = 0;
    };

    void processChunk (juce::dsp::AudioBlock<float>& block, int startSample, int numSamples) noexcept;
    void adoptPendingIfReady() noexcept;
    void transformIntoSpectra (const float* impulse, int numSamples, FilterSpectra& destination);
    void accumulate (const FilterSpectra& spectra, std::vector<float>& accumulator) noexcept;

    std::unique_ptr<juce::dsp::FFT> fft;

    int partitionSize = 0;    // B0: samples consumed per FFT hop
    int fftSize = 0;          // K = 2 * B0
    int fftOrder = 0;
    int numBins = 0;
    int maxPartitions = 0;
    int numChannels = 0;
    int maximumBlockSize = 0;

    // The channel accumulate() is currently summing for. Passing it as a
    // member rather than an argument keeps the inner MAC loop's signature
    // (and its register pressure) minimal; processChunk() is the only writer
    // and it is strictly single-threaded.
    int activeChannel = 0;

    // The shared frequency-domain delay line of input spectra: a ring of
    // `maxPartitions` interleaved spectra per channel. Shared by both filter
    // sets, which is precisely why an exchange needs no state reset.
    std::vector<std::vector<float>> inputSpectra; // per channel
    int fdlWriteIndex = 0;

    // Time-domain input history per channel: the previous partition's samples
    // followed by the current one, re-transformed every hop (overlap-save).
    std::vector<std::vector<float>> inputHistory;
    int historyFill = 0;

    // Output overlap accumulators, one per channel per filter set.
    std::vector<std::vector<float>> currentOutput;
    std::vector<std::vector<float>> nextOutput;

    // Scratch, all preallocated in prepare().
    std::vector<float> fftScratch;
    std::vector<float> accumulatorScratch;
    std::vector<float> transformScratch;

    FilterSpectra currentSpectra;
    FilterSpectra nextSpectra;
    FilterSpectra pendingSpectra;

    std::atomic<bool> pendingAvailable { false };
    std::atomic<bool> pendingBusy { false };

    int crossfadeLengthSamples = 0;
    int crossfadeSamplesRemaining = 0;

    int numActivePartitions = 0;
    bool anyImpulseLoaded = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphConvolver)
};
