#pragma once

#include "FractionalDelay.h"
#include "MinPhase.h"
#include "MorphConvolver.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// Mic-position morphing between two impulse responses.
//
// THE PROBLEM. Crossfading two convolvers - what Blend does in Crossfade mode,
// and what every other open IR loader does - sums two signals whose direct
// arrivals are at different times. Two captures of the same cabinet a few
// centimetres apart differ by a fraction of a millisecond, and summing them
// combs: a deep notch wherever the offset is half a wavelength. At 50/50 the
// notch is at its worst, which is exactly where a user looking for "between
// these two mics" lands. Aligning the onsets (what v0.2 does) fixes the
// deepest notch but not the residual excess-phase difference across the band.
//
// THE FIX. Decompose each IR into (minimum-phase magnitude) + (bulk delay),
// interpolate those two components separately, and resynthesise:
//
//   logMag = (1-b).logMag_A + b.logMag_B     interpolate the MAGNITUDE in log
//   tau    = (1-b).tau_A    + b.tau_B        interpolate the TIMING separately
//   h      = minPhase(exp(logMag)) delayed by tau
//
// There is only ever ONE impulse response in the signal path, so there is
// nothing to comb against. Interpolating in the log domain is a geometric
// rather than arithmetic mean, so a resonance both IRs share keeps its level
// at b = 0.5 instead of dipping. Interpolating tau separately means the
// morph glides in time like a mic physically moving, rather than
// phase-cancelling its way between two fixed positions. This is the same
// decomposition Two Notes and Fractal use internally.
//
// THE COST is that resynthesis is far too expensive for the audio thread
// (several FFTs over up to 32768 points). It therefore runs on a dedicated
// worker thread, woken by a condition variable whenever Blend moves, and the
// result is handed to MorphConvolver's lock-free publish path. The worker
// coalesces: if Blend moves three times while one resynthesis is in flight,
// only the newest value is computed next. The audio thread never waits for
// it - it simply keeps running the last published IR until a new one arrives,
// which at up to 30 updates a second is imperceptible.
//
// ENDPOINT SEMANTICS. Morph mode minimum-phases both endpoints by
// construction, so b = 0 is MPT(A) delayed by tau_A, not raw A. That is the
// same choice Fractal's MPT makes, and it is why blendMode defaults to
// Crossfade: switching to Morph is an audible, opt-in change of character,
// never something an existing session acquires silently.
class MorphEngine
{
public:
    MorphEngine();
    ~MorphEngine();

    // Allocates analysis/resynthesis scratch and starts the worker thread.
    // Not real-time safe. Safe to call repeatedly (re-prepare on sample-rate
    // change); the worker is reused.
    void prepare (const juce::dsp::ProcessSpec& spec);

    // Stops the worker thread and releases its resources. Called from the
    // destructor; exposed so tests can shut down deterministically.
    void releaseResources();

    // Sets the impulse response for a slot and re-runs its offline analysis
    // (log-magnitude spectrum + bulk delay). MUST be called off the audio
    // thread. Triggers a resynthesis at the current blend value.
    void setImpulseResponse (int slotIndex, const juce::AudioBuffer<float>& buffer);

    // The current blend position, 0 = slot A, 1 = slot B. Safe to call from
    // the audio thread every block: it only stores an atomic and (when the
    // value has moved far enough to matter) signals the worker.
    void setBlend (float newBlend01) noexcept;

    // The interpolated bulk delay for the current blend, in samples at the
    // prepared sample rate. The engine applies this itself inside process();
    // exposed for tests and for the CabConvolutionEngine's latency reasoning.
    float getInterpolatedDelaySamples() const noexcept
    {
        return interpolatedDelay.load (std::memory_order_relaxed);
    }

    // Processes `block` in place through the morph convolver and the
    // interpolated bulk delay. No allocation, no locks. If no morph IR has
    // been published yet the block is left untouched (passthrough).
    void process (juce::dsp::AudioBlock<float>& block) noexcept;

    // Clears the convolver and delay state. Safe on the audio thread.
    void reset() noexcept;

    // True once at least one morph IR has been published and adopted, i.e.
    // process() is actually doing something.
    bool isReady() const noexcept { return published.load (std::memory_order_acquire); }

    // Runs one resynthesis immediately on the calling thread instead of
    // waiting for the worker. Tests use this to make the whole morph path
    // deterministic; production code never calls it.
    void resynthesiseSynchronouslyForTesting();

    // Blocks until the worker has finished any pending resynthesis and the
    // convolver has adopted it. Test-only.
    void waitForWorkerForTesting();

    MorphConvolver& getConvolverForTesting() noexcept { return convolver; }

private:
    // Everything one slot contributes to the interpolation.
    //
    // The mono-summed, length-capped impulse is stored rather than a
    // precomputed log-magnitude spectrum, because the two slots' spectra can
    // only be interpolated bin by bin if they were transformed at the SAME FFT
    // order - and the right order depends on both IRs' lengths, which is not
    // known when a single slot is loaded. Deriving the common order at
    // resynthesis time and transforming both slots there costs two extra FFTs
    // per update (at most 30 a second, off the audio thread) and removes an
    // entire class of "the two slots disagree about their analysis size" bugs.
    //
    // The bulk delay is order-independent and so is computed once, at load.
    struct SlotAnalysis
    {
        std::vector<float> preparedImpulse;
        float bulkDelaySamples = 0.0f;
        bool valid = false;
    };

    void workerLoop();
    void resynthesise (float blend);
    void notifyWorker();

    MorphConvolver convolver;
    FractionalDelay delayLine;

    double sampleRate = 44100.0;

    SlotAnalysis slots[2];

    // Guards slots[] against concurrent read (worker) and write
    // (message-thread IR load). Never taken on the audio thread.
    std::mutex analysisMutex;

    std::atomic<float> requestedBlend { 0.0f };
    std::atomic<float> interpolatedDelay { 0.0f };
    std::atomic<bool> published { false };

    // Worker signalling. `workPending` is the coalescing flag: several blend
    // moves while a resynthesis is in flight collapse into one follow-up pass.
    std::mutex workMutex;
    std::condition_variable workCondition;
    bool workPending = false;
    bool shouldExit = false;
    juce::uint32 workGeneration = 0;
    juce::uint32 completedGeneration = 0;

    std::thread worker;
    bool workerRunning = false;

    // The blend value the last resynthesis was computed at, used to throttle:
    // a move smaller than this threshold cannot produce an audibly different
    // IR, so it is not worth a resynthesis.
    float lastResynthesisedBlend = -1.0f;
    static constexpr float blendResynthesisThreshold = 0.002f;

    // Worker-side scratch, sized to the worst case in prepare() so the worker
    // does not reallocate in steady state.
    std::vector<float> logMagnitudeA;
    std::vector<float> logMagnitudeB;
    std::vector<float> interpolatedLogMagnitude;
    std::vector<float> resynthesisedImpulse;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MorphEngine)
};
