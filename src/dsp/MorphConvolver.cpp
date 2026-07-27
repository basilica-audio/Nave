#include "MorphConvolver.h"

#include <algorithm>
#include <thread>

namespace
{
    // The FFT hop. 512 is the sweet spot for cabinet IRs: large enough that a
    // 16k-tap IR needs only 32 partitions (so the per-hop MAC chain stays
    // cheap), small enough that the per-callback re-transform the zero-latency
    // scheme requires stays affordable at small host block sizes.
    constexpr int preferredPartitionSize = 512;

    int nextPowerOfTwoAtLeast (int value) noexcept
    {
        int result = 1;

        while (result < value)
            result <<= 1;

        return result;
    }
}

void MorphConvolver::prepare (const juce::dsp::ProcessSpec& spec)
{
    numChannels = juce::jmax (1, static_cast<int> (spec.numChannels));
    maximumBlockSize = juce::jmax (1, static_cast<int> (spec.maximumBlockSize));

    // B0 = nextPow2(min(maxBlockSize, 512)). Capping at the host's block size
    // means a host running tiny blocks doesn't pay for a needlessly long FFT
    // on every callback.
    partitionSize = nextPowerOfTwoAtLeast (juce::jmin (maximumBlockSize, preferredPartitionSize));
    partitionSize = juce::jmax (32, partitionSize);

    fftSize = partitionSize * 2;

    fftOrder = 0;

    while ((1 << fftOrder) < fftSize)
        ++fftOrder;

    numBins = fftSize / 2 + 1;

    // Allocated here, never on the audio thread.
    fft = std::make_unique<juce::dsp::FFT> (fftOrder);

    maxPartitions = (maxImpulseLength + partitionSize - 1) / partitionSize;

    const auto spectrumFloats = static_cast<size_t> (fftSize) * 2;

    inputSpectra.assign (static_cast<size_t> (numChannels),
                          std::vector<float> (spectrumFloats * static_cast<size_t> (maxPartitions), 0.0f));

    inputHistory.assign (static_cast<size_t> (numChannels),
                          std::vector<float> (static_cast<size_t> (fftSize), 0.0f));

    currentOutput.assign (static_cast<size_t> (numChannels),
                           std::vector<float> (spectrumFloats, 0.0f));
    nextOutput.assign (static_cast<size_t> (numChannels),
                        std::vector<float> (spectrumFloats, 0.0f));

    fftScratch.assign (spectrumFloats, 0.0f);
    accumulatorScratch.assign (spectrumFloats, 0.0f);
    transformScratch.assign (spectrumFloats, 0.0f);

    const auto filterFloats = spectrumFloats * static_cast<size_t> (maxPartitions);

    currentSpectra.data.assign (filterFloats, 0.0f);
    nextSpectra.data.assign (filterFloats, 0.0f);
    pendingSpectra.data.assign (filterFloats, 0.0f);

    currentSpectra.numPartitions = 0;
    nextSpectra.numPartitions = 0;
    pendingSpectra.numPartitions = 0;

    pendingAvailable.store (false, std::memory_order_release);
    pendingBusy.store (false, std::memory_order_release);

    crossfadeLengthSamples = juce::jmax (1, static_cast<int> (crossfadeSeconds * spec.sampleRate));

    numActivePartitions = 0;
    anyImpulseLoaded = false;

    reset();
}

void MorphConvolver::reset() noexcept
{
    for (auto& history : inputHistory)
        std::fill (history.begin(), history.end(), 0.0f);

    for (auto& spectra : inputSpectra)
        std::fill (spectra.begin(), spectra.end(), 0.0f);

    for (auto& output : currentOutput)
        std::fill (output.begin(), output.end(), 0.0f);

    for (auto& output : nextOutput)
        std::fill (output.begin(), output.end(), 0.0f);

    fdlWriteIndex = 0;
    historyFill = 0;

    // An in-flight crossfade is abandoned rather than finished: reset() means
    // "the signal is discontinuous anyway" (transport stop, re-prepare), so
    // the newest IR should simply become the live one immediately.
    if (crossfadeSamplesRemaining > 0)
    {
        std::swap (currentSpectra, nextSpectra);
        numActivePartitions = currentSpectra.numPartitions;
        crossfadeSamplesRemaining = 0;
    }
}

bool MorphConvolver::publishImpulseResponse (const float* impulse, int numSamples)
{
    if (fft == nullptr || impulse == nullptr || numSamples <= 0)
        return false;

    numSamples = juce::jmin (numSamples, maxImpulseLength);

    // Claim the pending slot. Only the (single) worker thread ever spins here;
    // the audio thread's side of this handshake is a non-blocking try (see
    // adoptPendingIfReady), so a publication in progress can never stall a
    // callback.
    bool expected = false;

    while (! pendingBusy.compare_exchange_weak (expected, true,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
    {
        expected = false;
        std::this_thread::yield();
    }

    transformIntoSpectra (impulse, numSamples, pendingSpectra);

    // Latest-wins: setting this unconditionally overwrites any previous
    // unconsumed publication, so dragging Blend never builds a backlog of
    // stale intermediate IRs the engine would have to chew through.
    pendingAvailable.store (true, std::memory_order_release);
    pendingBusy.store (false, std::memory_order_release);

    return true;
}

void MorphConvolver::transformIntoSpectra (const float* impulse, int numSamples, FilterSpectra& destination)
{
    const auto numPartitions = juce::jmin (maxPartitions,
                                            (numSamples + partitionSize - 1) / partitionSize);

    const auto spectrumFloats = static_cast<size_t> (fftSize) * 2;

    for (int partition = 0; partition < numPartitions; ++partition)
    {
        auto* target = destination.data.data() + static_cast<size_t> (partition) * spectrumFloats;

        std::fill (target, target + spectrumFloats, 0.0f);

        // Partition p holds IR taps [p*B, (p+1)*B), zero-padded to K - the
        // uniform-partition layout the overlap-save sum in accumulate()
        // assumes.
        const auto offset = partition * partitionSize;
        const auto count = juce::jmin (partitionSize, numSamples - offset);

        std::copy_n (impulse + offset, count, target);

        fft->performRealOnlyForwardTransform (target, true);
    }

    destination.numPartitions = numPartitions;
}

void MorphConvolver::adoptPendingIfReady() noexcept
{
    if (! pendingAvailable.load (std::memory_order_acquire))
        return;

    // A crossfade already running means the previous exchange has not
    // finished. Leave the publication pending rather than truncating that
    // fade; it will be adopted the moment the fade completes.
    if (crossfadeSamplesRemaining > 0)
        return;

    bool expected = false;

    // Non-blocking: if the worker happens to be mid-write, skip this block and
    // try again on the next one. The audio thread never waits.
    if (! pendingBusy.compare_exchange_strong (expected, true,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed))
        return;

    std::swap (nextSpectra, pendingSpectra);

    pendingAvailable.store (false, std::memory_order_release);
    pendingBusy.store (false, std::memory_order_release);

    if (! anyImpulseLoaded)
    {
        // Nothing is playing through this convolver yet, so there is no
        // discontinuity to hide - adopt the very first IR outright rather than
        // fading up from silence.
        std::swap (currentSpectra, nextSpectra);
        numActivePartitions = currentSpectra.numPartitions;
        anyImpulseLoaded = true;
        return;
    }

    crossfadeSamplesRemaining = crossfadeLengthSamples;
    numActivePartitions = juce::jmax (currentSpectra.numPartitions, nextSpectra.numPartitions);
}

void MorphConvolver::accumulate (const FilterSpectra& spectra, std::vector<float>& accumulator) noexcept
{
    const auto spectrumFloats = static_cast<size_t> (fftSize) * 2;

    std::fill (accumulator.begin(), accumulator.end(), 0.0f);

    const auto* channelSpectra = inputSpectra[static_cast<size_t> (activeChannel)].data();

    // Y_m(w) = sum_p X_{m-p}(w) . H_p(w) - the frequency-domain delay line
    // walked backwards from the newest input segment.
    for (int partition = 0; partition < spectra.numPartitions; ++partition)
    {
        auto slot = fdlWriteIndex - partition;

        while (slot < 0)
            slot += maxPartitions;

        const auto* x = channelSpectra + static_cast<size_t> (slot % maxPartitions) * spectrumFloats;
        const auto* h = spectra.data.data() + static_cast<size_t> (partition) * spectrumFloats;

        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto xr = x[static_cast<size_t> (bin) * 2];
            const auto xi = x[static_cast<size_t> (bin) * 2 + 1];
            const auto hr = h[static_cast<size_t> (bin) * 2];
            const auto hi = h[static_cast<size_t> (bin) * 2 + 1];

            accumulator[static_cast<size_t> (bin) * 2] += xr * hr - xi * hi;
            accumulator[static_cast<size_t> (bin) * 2 + 1] += xr * hi + xi * hr;
        }
    }
}

void MorphConvolver::process (juce::dsp::AudioBlock<float>& block) noexcept
{
    const auto totalSamples = static_cast<int> (block.getNumSamples());

    if (fft == nullptr || totalSamples <= 0)
        return;

    // Pick up a first IR published since the last callback, so the convolver
    // goes live at the earliest possible block rather than one late.
    adoptPendingIfReady();

    // With no IR ever published this convolver has nothing to say. Leaving the
    // block untouched (rather than silencing it) keeps it a passthrough,
    // matching the delta-IR default of the stock engines.
    if (! anyImpulseLoaded)
        return;

    int offset = 0;

    while (offset < totalSamples)
    {
        // Never consume past the end of the current partition in one go: the
        // hop bookkeeping below assumes a chunk lands entirely within it.
        const auto chunk = juce::jmin (totalSamples - offset, partitionSize - historyFill);

        processChunk (block, offset, chunk);

        offset += chunk;
    }
}

void MorphConvolver::processChunk (juce::dsp::AudioBlock<float>& block, int startSample, int numSamples) noexcept
{
    if (numSamples <= 0)
        return;

    adoptPendingIfReady();

    const auto fading = crossfadeSamplesRemaining > 0;
    const auto blockChannels = juce::jmin (static_cast<int> (block.getNumChannels()), numChannels);

    const auto spectrumFloats = static_cast<size_t> (fftSize) * 2;

    for (int channel = 0; channel < blockChannels; ++channel)
    {
        activeChannel = channel;

        auto& history = inputHistory[static_cast<size_t> (channel)];
        const auto* input = block.getChannelPointer (static_cast<size_t> (channel)) + startSample;

        // The window is [previous partition | current partition]; the current
        // half is filled progressively and everything beyond the samples
        // received so far is held at zero. Because the filter is causal,
        // zeroing the not-yet-known future cannot affect any output sample at
        // or before the newest input - which is exactly what makes this scheme
        // zero-latency rather than merely low-latency.
        std::copy_n (input, numSamples, history.begin() + partitionSize + historyFill);

        std::fill (history.begin() + partitionSize + historyFill + numSamples,
                    history.begin() + fftSize,
                    0.0f);

        std::copy (history.begin(), history.begin() + fftSize, fftScratch.begin());
        std::fill (fftScratch.begin() + fftSize, fftScratch.end(), 0.0f);

        fft->performRealOnlyForwardTransform (fftScratch.data(), true);

        // Re-transforming into the *same* FDL slot each callback is what lets
        // a partially-filled partition produce output immediately: the slot
        // always holds the best current estimate of this segment's spectrum.
        auto* slot = inputSpectra[static_cast<size_t> (channel)].data()
                      + static_cast<size_t> (fdlWriteIndex) * spectrumFloats;

        std::copy_n (fftScratch.begin(), spectrumFloats, slot);

        accumulate (currentSpectra, accumulatorScratch);
        std::copy_n (accumulatorScratch.begin(), spectrumFloats,
                      currentOutput[static_cast<size_t> (channel)].begin());
        fft->performRealOnlyInverseTransform (currentOutput[static_cast<size_t> (channel)].data());

        if (fading)
        {
            accumulate (nextSpectra, accumulatorScratch);
            std::copy_n (accumulatorScratch.begin(), spectrumFloats,
                          nextOutput[static_cast<size_t> (channel)].begin());
            fft->performRealOnlyInverseTransform (nextOutput[static_cast<size_t> (channel)].data());
        }
    }

    // Overlap-save: the first B samples of the IFFT are circular-wrap garbage;
    // the last B are the valid linear-convolution output for the current
    // segment. Sample j of the current segment therefore lives at index
    // partitionSize + j.
    const auto inverseLength = crossfadeLengthSamples > 0
                                    ? 1.0f / static_cast<float> (crossfadeLengthSamples)
                                    : 0.0f;

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto readIndex = static_cast<size_t> (partitionSize + historyFill + sample);

        float gainNext = 0.0f;

        if (fading)
        {
            const auto remaining = crossfadeSamplesRemaining - sample;

            // Amplitude-complementary gains: g_next + g_current == 1 exactly,
            // at every point. See the class docs for why equal-power gains
            // would be wrong here.
            gainNext = remaining > 0
                            ? 1.0f - static_cast<float> (remaining) * inverseLength
                            : 1.0f;
        }

        for (int channel = 0; channel < blockChannels; ++channel)
        {
            auto* output = block.getChannelPointer (static_cast<size_t> (channel)) + startSample;

            const auto currentValue = currentOutput[static_cast<size_t> (channel)][readIndex];

            output[sample] = fading
                                  ? currentValue * (1.0f - gainNext)
                                        + nextOutput[static_cast<size_t> (channel)][readIndex] * gainNext
                                  : currentValue;
        }
    }

    if (fading)
    {
        crossfadeSamplesRemaining = juce::jmax (0, crossfadeSamplesRemaining - numSamples);

        if (crossfadeSamplesRemaining == 0)
        {
            std::swap (currentSpectra, nextSpectra);
            numActivePartitions = currentSpectra.numPartitions;
        }
    }

    historyFill += numSamples;

    if (historyFill >= partitionSize)
    {
        // The current partition is complete: it becomes the previous one, the
        // FDL advances, and the next segment starts filling from zero.
        for (auto& history : inputHistory)
        {
            std::copy (history.begin() + partitionSize, history.begin() + fftSize, history.begin());
            std::fill (history.begin() + partitionSize, history.begin() + fftSize, 0.0f);
        }

        historyFill = 0;

        // The slot this advances onto currently holds the spectrum of a
        // segment maxPartitions hops ago - i.e. exactly one past the oldest
        // partition the MAC chain reads - so it is overwritten by the next
        // chunk before it can ever be summed. No clearing needed.
        fdlWriteIndex = (fdlWriteIndex + 1) % maxPartitions;
    }
}
