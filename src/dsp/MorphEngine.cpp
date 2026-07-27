#include "MorphEngine.h"

#include <algorithm>
#include <cmath>

namespace
{
    // The morph resynthesis rate cap. 30 Hz is fast enough that a Blend drag
    // feels continuous (each update is crossfaded over 30 ms, so consecutive
    // updates overlap into one smooth glide) and slow enough that the worker
    // never saturates a core.
    constexpr int minimumMillisecondsBetweenUpdates = 33;

    // The bulk delay a morph IR can carry, in samples at 96 kHz - about 21 ms,
    // far beyond any real mic-position difference, so the delay line never
    // clamps in practice.
    constexpr int maxBulkDelaySamples = 2048;
}

MorphEngine::MorphEngine() = default;

MorphEngine::~MorphEngine()
{
    releaseResources();
}

void MorphEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;

    convolver.prepare (spec);

    delayLine.prepare (spec.sampleRate, static_cast<int> (spec.numChannels), maxBulkDelaySamples);
    delayLine.setSmoothingTimeSeconds (0.05);

    // Worst-case sizes, so the worker never allocates once it is running.
    const auto maxBins = MinPhase::maxAnalysisSize / 2 + 1;

    logMagnitudeA.reserve (static_cast<size_t> (maxBins));
    logMagnitudeB.reserve (static_cast<size_t> (maxBins));
    interpolatedLogMagnitude.assign (static_cast<size_t> (maxBins), 0.0f);
    resynthesisedImpulse.assign (static_cast<size_t> (MorphConvolver::maxImpulseLength), 0.0f);

    published.store (false, std::memory_order_release);
    lastResynthesisedBlend = -1.0f;

    if (! workerRunning)
    {
        shouldExit = false;
        worker = std::thread ([this] { workerLoop(); });
        workerRunning = true;
    }

    // Re-analysis is not needed (the slots' spectra are sample-rate
    // independent), but the convolver was just re-prepared and has no IR, so
    // ask for a fresh publication at the current blend.
    notifyWorker();
}

void MorphEngine::releaseResources()
{
    if (! workerRunning)
        return;

    {
        std::lock_guard<std::mutex> lock (workMutex);
        shouldExit = true;
        workPending = true;
    }

    workCondition.notify_all();

    if (worker.joinable())
        worker.join();

    workerRunning = false;
}

void MorphEngine::setImpulseResponse (int slotIndex, const juce::AudioBuffer<float>& buffer)
{
    if (slotIndex < 0 || slotIndex > 1)
        return;

    // The expensive analysis is done outside the lock so an IR load never
    // blocks the worker for longer than the handoff itself.
    SlotAnalysis analysis;

    analysis.preparedImpulse = MinPhase::prepareForAnalysis (buffer);

    if (! analysis.preparedImpulse.empty())
    {
        // The bulk delay is what carries this mic's physical distance from the
        // cone into the interpolation - see the header's decomposition note.
        // It is a property of the IR alone, so unlike the log-magnitude
        // spectrum it does not depend on the analysis order and can be
        // computed once here.
        analysis.bulkDelaySamples = MinPhase::estimateBulkDelaySamples (buffer);
        analysis.valid = true;
    }

    {
        std::lock_guard<std::mutex> lock (analysisMutex);
        slots[static_cast<size_t> (slotIndex)] = std::move (analysis);
    }

    // Force a resynthesis even if Blend has not moved: the IR behind it has.
    lastResynthesisedBlend = -1.0f;
    notifyWorker();
}

void MorphEngine::setBlend (float newBlend01) noexcept
{
    const auto clamped = juce::jlimit (0.0f, 1.0f, newBlend01);

    const auto previous = requestedBlend.exchange (clamped, std::memory_order_relaxed);

    // Throttle at the source: a move too small to change the resynthesised IR
    // audibly is not worth waking the worker for. This is what keeps a slow
    // automation ramp from requesting an update on literally every block.
    if (std::abs (clamped - previous) < blendResynthesisThreshold
        && lastResynthesisedBlend >= 0.0f
        && std::abs (clamped - lastResynthesisedBlend) < blendResynthesisThreshold)
        return;

    notifyWorker();
}

void MorphEngine::notifyWorker()
{
    {
        // try_lock, not lock: notifyWorker() is reachable from setBlend(),
        // which the audio thread calls. Skipping a notification is harmless -
        // the next block will try again - whereas blocking the audio thread on
        // a mutex the worker might hold is not.
        std::unique_lock<std::mutex> lock (workMutex, std::try_to_lock);

        if (! lock.owns_lock())
            return;

        workPending = true;
        ++workGeneration;
    }

    workCondition.notify_one();
}

void MorphEngine::workerLoop()
{
    juce::Thread::setCurrentThreadName ("Nave Morph");

    while (true)
    {
        juce::uint32 generation = 0;

        {
            std::unique_lock<std::mutex> lock (workMutex);
            workCondition.wait (lock, [this] { return workPending || shouldExit; });

            if (shouldExit)
                return;

            workPending = false;
            generation = workGeneration;
        }

        // Coalescing: the blend value is read *now*, after waking, so every
        // intermediate value that arrived while the previous resynthesis was
        // running is skipped and only the newest one is computed.
        resynthesise (requestedBlend.load (std::memory_order_relaxed));

        completedGeneration = generation;

        // Rate limit. Sleeping here rather than before the work means a single
        // isolated blend move is serviced immediately and only a *sustained*
        // drag is throttled.
        std::this_thread::sleep_for (std::chrono::milliseconds (minimumMillisecondsBetweenUpdates));
    }
}

void MorphEngine::resynthesise (float blend)
{
    std::lock_guard<std::mutex> lock (analysisMutex);

    const auto& slotA = slots[0];
    const auto& slotB = slots[1];

    // With only one slot analysed, morph degenerates to that slot alone rather
    // than refusing to produce anything - the user still hears their IR.
    const auto* primary = slotA.valid ? &slotA : (slotB.valid ? &slotB : nullptr);

    if (primary == nullptr)
        return;

    const auto interpolable = slotA.valid && slotB.valid;

    const auto lengthA = static_cast<int> (slotA.preparedImpulse.size());
    const auto lengthB = static_cast<int> (slotB.preparedImpulse.size());

    const auto irLength = interpolable ? juce::jmax (lengthA, lengthB)
                                        : static_cast<int> (primary->preparedImpulse.size());

    if (irLength <= 0)
        return;

    // ONE analysis order for both slots, derived from the longer of the two.
    // Transforming here rather than at load time is what guarantees the two
    // spectra have identical bin counts and identical bin-to-frequency
    // mapping, which bin-wise interpolation below requires.
    const auto fftOrder = MinPhase::chooseAnalysisOrder (irLength);

    float delaySamples = 0.0f;

    if (interpolable)
    {
        MinPhase::computeLogMagnitude (slotA.preparedImpulse, fftOrder, logMagnitudeA);
        MinPhase::computeLogMagnitude (slotB.preparedImpulse, fftOrder, logMagnitudeB);

        const auto numBins = logMagnitudeA.size();
        interpolatedLogMagnitude.resize (numBins);

        const auto b = juce::jlimit (0.0f, 1.0f, blend);
        const auto a = 1.0f - b;

        // Linear interpolation of the LOG magnitude, i.e. a geometric mean of
        // the magnitudes. An arithmetic mean would dip at any frequency where
        // the two IRs disagree in level; the geometric mean tracks the shared
        // resonance instead. This is the core of the morph.
        for (size_t bin = 0; bin < numBins; ++bin)
            interpolatedLogMagnitude[bin] = a * logMagnitudeA[bin] + b * logMagnitudeB[bin];

        delaySamples = a * slotA.bulkDelaySamples + b * slotB.bulkDelaySamples;
    }
    else
    {
        // Only one slot loaded: morph degenerates to that slot's own
        // minimum-phase equivalent rather than producing nothing.
        MinPhase::computeLogMagnitude (primary->preparedImpulse, fftOrder, interpolatedLogMagnitude);

        delaySamples = primary->bulkDelaySamples;
    }

    const auto outputLength = juce::jlimit (1, MorphConvolver::maxImpulseLength, irLength);

    MinPhase::resynthesiseFromLogMagnitude (interpolatedLogMagnitude, fftOrder,
                                             outputLength, resynthesisedImpulse);

    // The interpolated bulk delay is applied by the delay line on the audio
    // thread, not baked into the IR: a baked delay would have to be
    // re-published (and re-crossfaded) for every sub-sample change, whereas
    // the delay line glides continuously, which is what makes a Blend drag
    // sound like a mic moving.
    interpolatedDelay.store (juce::jlimit (0.0f, static_cast<float> (maxBulkDelaySamples), delaySamples),
                              std::memory_order_relaxed);

    if (convolver.publishImpulseResponse (resynthesisedImpulse.data(),
                                           juce::jmin (outputLength,
                                                        static_cast<int> (resynthesisedImpulse.size()))))
    {
        published.store (true, std::memory_order_release);
        lastResynthesisedBlend = blend;
    }
}

void MorphEngine::resynthesiseSynchronouslyForTesting()
{
    resynthesise (requestedBlend.load (std::memory_order_relaxed));
}

void MorphEngine::waitForWorkerForTesting()
{
    // Poll rather than condition-wait: the worker's completion flag is only
    // meaningful together with the convolver having actually adopted the
    // publication, and this is test-only code where simplicity beats
    // elegance.
    for (int attempt = 0; attempt < 500; ++attempt)
    {
        const auto pending = [this]
        {
            std::lock_guard<std::mutex> lock (workMutex);
            return workPending || completedGeneration != workGeneration;
        }();

        if (! pending && ! convolver.isExchangeInProgress())
            return;

        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }
}

void MorphEngine::reset() noexcept
{
    convolver.reset();
    delayLine.reset();
}

void MorphEngine::process (juce::dsp::AudioBlock<float>& block) noexcept
{
    if (! published.load (std::memory_order_acquire))
        return;

    convolver.process (block);

    const auto delaySamples = interpolatedDelay.load (std::memory_order_relaxed);

    // Below one sample the Lagrange interpolator cannot express the delay
    // anyway (it needs a tap either side of the read position), so the line is
    // skipped entirely rather than asked for something it would silently
    // round. Real mic-position differences are far larger than a sample.
    if (delaySamples >= 1.0f || delayLine.getCurrentDelaySamples() >= 1.0f)
    {
        delayLine.setDelaySamples (delaySamples);
        delayLine.process (block);
    }
}
