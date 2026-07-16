#include "CabConvolutionEngine.h"
#include "IrAlignment.h"

namespace
{
    // Keeps a requested filter frequency safely below Nyquist regardless of
    // host sample rate, so juce::dsp::IIR::Coefficients::makeHighPass/
    // makeLowPass never receives an out-of-range value (which would produce
    // invalid/NaN coefficients).
    float clampBelowNyquist (float frequencyHz, double sampleRate) noexcept
    {
        const auto nyquist = static_cast<float> (sampleRate) * 0.5f;
        return juce::jlimit (10.0f, nyquist * 0.9f, frequencyHz);
    }

    // A single-sample, unit-amplitude impulse response: the mathematical
    // identity for convolution (y = x * delta = x). Declaring it mono is
    // deliberate - juce::dsp::Convolution applies a mono IR identically to
    // every processed channel, so a 1-channel delta is sufficient regardless
    // of whether the host session is mono or stereo.
    juce::AudioBuffer<float> makeDeltaImpulseResponse()
    {
        juce::AudioBuffer<float> buffer (1, 1);
        buffer.setSample (0, 0, 1.0f);
        return buffer;
    }
}

CabConvolutionEngine::CabConvolutionEngine() = default;

void CabConvolutionEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    numChannelsPrepared = static_cast<int> (spec.numChannels);

    // Establish a valid IR before the first process() call. On the very
    // first prepare() (no IR ever loaded/requested yet), that means the
    // default delta/identity IR. On subsequent prepares (sample-rate
    // change, etc.) juce::dsp::Convolution retains and automatically
    // re-resamples whatever IR was most recently loaded, so nothing further
    // needs to be done here - see the class-level docs on
    // juce::dsp::Convolution::prepare() for this contract. Same story for
    // slot B.
    if (! anyImpulseResponseLoaded)
        loadDefaultImpulseResponse();

    if (! anyImpulseResponseBLoaded)
        loadDefaultImpulseResponseB();

    // Per juce::dsp::Convolution's documented contract: loadImpulseResponse()
    // must be called *before* prepare() for that IR to be guaranteed active
    // during the very first process() call.
    convolution.prepare (spec);
    convolutionB.prepare (spec);

    loCutFilter.prepare (spec);
    hiCutFilter.prepare (spec);
    distanceLowShelfFilter.prepare (spec);
    distanceHighShelfFilter.prepare (spec);

    // Not real-time safe (allocates) - fine here, prepare() is never called
    // from the audio thread. Never resized again in process().
    scratchBuffer.setSize (juce::jmax (1, numChannelsPrepared),
                            static_cast<int> (spec.maximumBlockSize),
                            false, false, true);

    // Prime the target gain from lastLevelDb *before* prepare() (which
    // internally calls reset(), snapping current == target) - otherwise a
    // freshly constructed engine's Level would default to silence (see
    // lastLevelDb's declaration in the header) rather than unity gain.
    outputLevel.setGainDecibels (lastLevelDb);
    outputLevel.setRampDurationSeconds (smoothingTimeSeconds);
    outputLevel.prepare (spec);

    dryWetMixer.prepare (spec);

    // Both convolution slots always use the same (default, zero-latency)
    // configuration, so in practice these are always equal - computed
    // generically via jmax so the dry path stays correctly compensated even
    // if a slot's configuration ever changes independently in future.
    latencySamples = juce::jmax (convolution.getLatency(), convolutionB.getLatency());
    dryWetMixer.setWetLatency (static_cast<float> (latencySamples));

    // juce::dsp::DryWetMixer defaults its internal mix to fully wet (1.0)
    // until told otherwise, and its own reset() (called from our reset()
    // below) snaps its internal dry/wet gain smoothers' *current* value to
    // whatever their *target* happens to be at that moment - it does not
    // know about lastMixProportion. Priming the real target here, before
    // reset() runs, means the mixer is already sitting at the correct dry/
    // wet balance for the very first process() call instead of ramping up
    // from "fully wet" over its internal 50ms default ramp.
    dryWetMixer.setWetMixProportion (lastMixProportion);

    // Re-seed the smoothers at the new sample rate, but pin current ==
    // target to whatever was last requested (defaulting to the
    // ParameterLayout defaults on first prepare) - otherwise the ramp would
    // sweep up from a default-constructed 0 Hz/0.0 on the very first block.
    loCutFrequencySmoothed.reset (sampleRate, smoothingTimeSeconds);
    loCutFrequencySmoothed.setCurrentAndTargetValue (lastLoCutHz);
    hiCutFrequencySmoothed.reset (sampleRate, smoothingTimeSeconds);
    hiCutFrequencySmoothed.setCurrentAndTargetValue (lastHiCutHz);
    mixSmoothed.reset (sampleRate, smoothingTimeSeconds);
    mixSmoothed.setCurrentAndTargetValue (lastMixProportion);
    blendSmoothed.reset (sampleRate, smoothingTimeSeconds);
    blendSmoothed.setCurrentAndTargetValue (lastBlendProportion);
    distanceSmoothed.reset (sampleRate, smoothingTimeSeconds);
    distanceSmoothed.setCurrentAndTargetValue (lastDistancePercent);

    reset();

    // Prime the filter coefficients immediately (only meaningful if not
    // bypassed - see process()) so a subsequent engage of the filter starts
    // from correct, non-default coefficients rather than an identity/
    // uninitialised state.
    *loCutFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (
        sampleRate, clampBelowNyquist (lastLoCutHz, sampleRate), filterQ);
    *hiCutFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (
        sampleRate, clampBelowNyquist (lastHiCutHz, sampleRate), filterQ);

    const auto normalisedDistance = (lastDistancePercent - distanceMinPercent)
                                     / (distanceMaxPercent - distanceMinPercent);
    *distanceLowShelfFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf (
        sampleRate, distanceLowShelfFrequencyHz, filterQ,
        juce::Decibels::decibelsToGain (normalisedDistance * distanceLowShelfMaxCutDb));
    *distanceHighShelfFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf (
        sampleRate, distanceHighShelfFrequencyHz, filterQ,
        juce::Decibels::decibelsToGain (normalisedDistance * distanceHighShelfMaxCutDb));

    loCutEngagedPreviously = lastLoCutHz > loCutMinHz + bypassEpsilonHz;
    hiCutEngagedPreviously = lastHiCutHz < hiCutMaxHz - bypassEpsilonHz;
    distanceEngagedPreviously = lastDistancePercent > distanceMinPercent + distanceBypassEpsilonPercent;
    blendEngagedPreviously = lastBlendProportion > blendBypassEpsilon;
}

void CabConvolutionEngine::reset()
{
    convolution.reset();
    convolutionB.reset();
    loCutFilter.reset();
    hiCutFilter.reset();
    distanceLowShelfFilter.reset();
    distanceHighShelfFilter.reset();
    outputLevel.reset();
    dryWetMixer.reset();
}

void CabConvolutionEngine::setLoCutHz (float newFrequencyHz)
{
    lastLoCutHz = newFrequencyHz;
    loCutFrequencySmoothed.setTargetValue (newFrequencyHz);
}

void CabConvolutionEngine::setHiCutHz (float newFrequencyHz)
{
    lastHiCutHz = newFrequencyHz;
    hiCutFrequencySmoothed.setTargetValue (newFrequencyHz);
}

void CabConvolutionEngine::setMixProportion (float newProportion01)
{
    lastMixProportion = newProportion01;
    mixSmoothed.setTargetValue (newProportion01);
}

void CabConvolutionEngine::setLevelDb (float newLevelDb)
{
    lastLevelDb = newLevelDb;
    outputLevel.setGainDecibels (newLevelDb);
}

void CabConvolutionEngine::setBlendProportion (float newProportion01)
{
    lastBlendProportion = newProportion01;
    blendSmoothed.setTargetValue (newProportion01);
}

void CabConvolutionEngine::setDistancePercent (float newDistancePercent)
{
    lastDistancePercent = newDistancePercent;
    distanceSmoothed.setTargetValue (newDistancePercent);
}

void CabConvolutionEngine::setImpulseResponse (juce::AudioBuffer<float> irBuffer, double irSampleRate)
{
    // Recorded before the buffer is moved from below: this becomes the
    // reference onset that a subsequently loaded IR B is phase-aligned
    // against (see setImpulseResponseB()).
    lastIrAOnsetSample = IrAlignment::detectOnsetSample (irBuffer);
    lastIrASampleRate = irSampleRate;

    const auto isStereo = (irBuffer.getNumChannels() >= 2 && numChannelsPrepared >= 2)
                               ? juce::dsp::Convolution::Stereo::yes
                               : juce::dsp::Convolution::Stereo::no;

    // Normalise::yes: JUCE scales the IR's energy to a consistent reference
    // level, so switching between wildly different real-world cabinet IRs
    // doesn't also produce wildly different output levels.
    convolution.loadImpulseResponse (std::move (irBuffer),
                                      irSampleRate,
                                      isStereo,
                                      juce::dsp::Convolution::Trim::no,
                                      juce::dsp::Convolution::Normalise::yes);

    anyImpulseResponseLoaded = true;

    // IR A's onset (the alignment reference recorded above) just changed. If
    // a real IR B is already loaded, its alignment was computed against the
    // *previous* reference and is now stale - silently reintroducing
    // comb-filtering the next time Blend crosses back into an engaged range
    // (see #13). Re-run alignment from IR B's retained raw buffer against
    // the new reference so it never goes stale like this.
    if (irBNeedsAlignment)
        setImpulseResponseB (lastIrBRawBuffer, lastIrBRawSampleRate);
}

void CabConvolutionEngine::loadDefaultImpulseResponse()
{
    // The delta IR's onset is trivially sample 0 - reset the phase-
    // alignment reference to match, at the engine's current sample rate.
    lastIrAOnsetSample = 0;
    lastIrASampleRate = sampleRate;

    // Normalise::no is essential here: normalising a unit impulse would
    // rescale it away from exact unity gain (JUCE's normalisation targets a
    // fixed reference energy, not "leave amplitude 1.0 alone"), which would
    // break the passthrough guarantee the default IR exists to provide.
    convolution.loadImpulseResponse (makeDeltaImpulseResponse(),
                                      sampleRate,
                                      juce::dsp::Convolution::Stereo::no,
                                      juce::dsp::Convolution::Trim::no,
                                      juce::dsp::Convolution::Normalise::no);

    anyImpulseResponseLoaded = true;

    // Same rationale as the end of setImpulseResponse() above: this changed
    // the alignment reference, so an already-loaded real IR B must be
    // re-aligned against it rather than left pointing at a stale onset (#13).
    if (irBNeedsAlignment)
        setImpulseResponseB (lastIrBRawBuffer, lastIrBRawSampleRate);
}

void CabConvolutionEngine::setImpulseResponseB (juce::AudioBuffer<float> irBuffer, double irSampleRate)
{
    // Retain a copy of the raw, pre-alignment buffer/rate so a later IR A
    // reload can re-run this alignment on its own (see setImpulseResponse()/
    // loadDefaultImpulseResponse() above and #13), without requiring the
    // caller to reload IR B. Copied (not moved) - `irBuffer` below still
    // needs its original content for the alignment call that follows.
    lastIrBRawBuffer.makeCopyOf (irBuffer);
    lastIrBRawSampleRate = irSampleRate;
    irBNeedsAlignment = true;

    // Inter-IR phase alignment: shift IR B's onset to match IR A's most
    // recently recorded onset, so blending the two convolution outputs
    // doesn't introduce comb-filtering from a timing mismatch between their
    // transients (see IrAlignment.h and docs/architecture.md).
    auto alignedIrBuffer = IrAlignment::alignOnsetToReference (
        irBuffer, irSampleRate, lastIrAOnsetSample, lastIrASampleRate);

    const auto isStereo = (alignedIrBuffer.getNumChannels() >= 2 && numChannelsPrepared >= 2)
                               ? juce::dsp::Convolution::Stereo::yes
                               : juce::dsp::Convolution::Stereo::no;

    convolutionB.loadImpulseResponse (std::move (alignedIrBuffer),
                                       irSampleRate,
                                       isStereo,
                                       juce::dsp::Convolution::Trim::no,
                                       juce::dsp::Convolution::Normalise::yes);

    anyImpulseResponseBLoaded = true;
}

void CabConvolutionEngine::loadDefaultImpulseResponseB()
{
    convolutionB.loadImpulseResponse (makeDeltaImpulseResponse(),
                                       sampleRate,
                                       juce::dsp::Convolution::Stereo::no,
                                       juce::dsp::Convolution::Trim::no,
                                       juce::dsp::Convolution::Normalise::no);

    anyImpulseResponseBLoaded = true;

    // The default delta IR has no meaningful onset to keep aligned - clear
    // the retained-raw-buffer bookkeeping so a subsequent IR A reload
    // doesn't try to re-align it (see setImpulseResponse() above and #13).
    irBNeedsAlignment = false;
    lastIrBRawBuffer.setSize (0, 0);
}

void CabConvolutionEngine::process (juce::dsp::AudioBlock<float>& block)
{
    const auto numSamples = block.getNumSamples();

    if (numSamples == 0)
        return;

    const auto numSamplesInt = static_cast<int> (numSamples);

    // Coefficient recomputation involves trig calls (tan/cos), so filter
    // frequencies are smoothed and re-derived once per block rather than
    // per sample - a standard real-time-safe compromise for IIR filters.
    const auto loCutHz = loCutFrequencySmoothed.skip (numSamplesInt);
    const auto hiCutHz = hiCutFrequencySmoothed.skip (numSamplesInt);
    const auto wetMix = mixSmoothed.skip (numSamplesInt);
    const auto blendProportion = blendSmoothed.skip (numSamplesInt);
    const auto distancePercent = distanceSmoothed.skip (numSamplesInt);

    // LoCut at its minimum, HiCut at its maximum, and Distance at its
    // minimum are each an explicit "off" position: skip the relevant IIR
    // processing entirely (rather than merely computing an extreme-but-
    // active cutoff/gain) so the default/wide-open state is a true
    // bit-accurate passthrough, not just negligible colouration. This is
    // what tests/EngineTests.cpp's null tests rely on.
    const bool loCutBypassed = loCutHz <= loCutMinHz + bypassEpsilonHz;
    const bool hiCutBypassed = hiCutHz >= hiCutMaxHz - bypassEpsilonHz;
    const bool distanceBypassed = distancePercent <= distanceMinPercent + distanceBypassEpsilonPercent;

    // Defensive fallback: scratchBuffer is sized to maximumBlockSize in
    // prepare(), so a host that (against its own promise) sends a larger
    // block here would overrun it. Rather than risk that, Blend is simply
    // treated as disengaged for that one block (falls back to IR A only) -
    // safer than allocating or writing out of bounds on the audio thread.
    const bool scratchLargeEnough = numSamples <= static_cast<size_t> (scratchBuffer.getNumSamples());
    const bool blendEngaged = blendProportion > blendBypassEpsilon && scratchLargeEnough;

    // Reset a filter's IIR state exactly when it transitions from bypassed
    // to engaged, so it starts from a clean, predictable state rather than
    // reusing whatever memory it was last left in an arbitrary number of
    // blocks ago.
    if (! loCutBypassed && ! loCutEngagedPreviously)
        loCutFilter.reset();

    if (! hiCutBypassed && ! hiCutEngagedPreviously)
        hiCutFilter.reset();

    if (! distanceBypassed && ! distanceEngagedPreviously)
    {
        distanceLowShelfFilter.reset();
        distanceHighShelfFilter.reset();
    }

    // Same idea for convolutionB: it keeps no history of its own bypass
    // state, so without this it's the one exception in this function that
    // never gets reset on a disengaged->engaged transition (see #12).
    // convolutionB.process() is skipped entirely for every block Blend is
    // disengaged (below), which freezes its internal overlap-add buffer
    // rather than decaying it - left unreset, that stale, time-decoupled
    // tail would be added back into the output the moment Blend re-engages.
    // juce::dsp::Convolution::reset() is documented noexcept/real-time safe
    // (JUCE 8.0.14 juce_Convolution.h) and this engine already calls it from
    // the audio thread via CabConvolutionEngine::reset(), so this is safe
    // here too.
    if (blendEngaged && ! blendEngagedPreviously)
        convolutionB.reset();

    loCutEngagedPreviously = ! loCutBypassed;
    hiCutEngagedPreviously = ! hiCutBypassed;
    blendEngagedPreviously = blendEngaged;
    distanceEngagedPreviously = ! distanceBypassed;

    if (! loCutBypassed)
        *loCutFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, clampBelowNyquist (loCutHz, sampleRate), filterQ);

    if (! hiCutBypassed)
        *hiCutFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowPass (sampleRate, clampBelowNyquist (hiCutHz, sampleRate), filterQ);

    if (! distanceBypassed)
    {
        const auto normalisedDistance = (distancePercent - distanceMinPercent) / (distanceMaxPercent - distanceMinPercent);

        *distanceLowShelfFilter.state = *juce::dsp::IIR::Coefficients<float>::makeLowShelf (
            sampleRate, distanceLowShelfFrequencyHz, filterQ,
            juce::Decibels::decibelsToGain (normalisedDistance * distanceLowShelfMaxCutDb));
        *distanceHighShelfFilter.state = *juce::dsp::IIR::Coefficients<float>::makeHighShelf (
            sampleRate, distanceHighShelfFrequencyHz, filterQ,
            juce::Decibels::decibelsToGain (normalisedDistance * distanceHighShelfMaxCutDb));
    }

    dryWetMixer.setWetMixProportion (wetMix);

    juce::dsp::ProcessContextReplacing<float> context (block);

    // Capture the pre-processing signal as "dry" before convolution or
    // filtering touches `block`. DryWetMixer internally delays this by
    // getLatencySamples() (set via setWetLatency in prepare()) so it stays
    // time-aligned with the wet path below, whatever that latency is.
    dryWetMixer.pushDrySamples (block);

    // Convolution stage: IR A always runs (needed both standalone and as
    // the (1 - blend) component of the crossfade below). IR B only runs
    // when Blend is actually engaged, saving the second convolution's CPU
    // cost otherwise - the same "skip work that provably can't matter"
    // pattern LoCut/HiCut/Distance use above.
    //
    // IR B must convolve the same original (dry) input as IR A, not IR A's
    // already-convolved output - so the pre-convolution samples are copied
    // into scratchBuffer *before* convolution.process() mutates `block` in
    // place. Getting this ordering wrong would silently turn the "B"
    // component of the crossfade into IR_B(IR_A(input)), a cascaded double
    // convolution, instead of the intended IR_B(input).
    if (blendEngaged)
    {
        juce::dsp::AudioBlock<float> scratchBlock (scratchBuffer);
        auto scratchSub = scratchBlock.getSubBlock (0, numSamples);
        scratchSub.copyFrom (block);
    }

    convolution.process (context);

    if (blendEngaged)
    {
        juce::dsp::AudioBlock<float> scratchBlock (scratchBuffer);
        auto scratchSub = scratchBlock.getSubBlock (0, numSamples);

        juce::dsp::ProcessContextReplacing<float> contextB (scratchSub);
        convolutionB.process (contextB);

        for (size_t channel = 0; channel < block.getNumChannels(); ++channel)
        {
            auto* a = block.getChannelPointer (channel);
            const auto* b = scratchSub.getChannelPointer (channel);

            for (size_t sample = 0; sample < numSamples; ++sample)
                a[sample] = a[sample] * (1.0f - blendProportion) + b[sample] * blendProportion;
        }
    }

    if (! distanceBypassed)
    {
        distanceLowShelfFilter.process (context);
        distanceHighShelfFilter.process (context);
    }

    if (! loCutBypassed)
        loCutFilter.process (context);

    if (! hiCutBypassed)
        hiCutFilter.process (context);

    dryWetMixer.mixWetSamples (block);

    outputLevel.process (context);
}
