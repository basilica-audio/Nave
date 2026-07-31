#include "CabConvolutionEngine.h"
#include "IrAlignment.h"
#include "IrLoudness.h"
#include "MinPhase.h"

#include <cmath>

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

    // v0.2.0 Distance taper (design-brief.md): "ease-out" power curve -
    // applies the exponent to the *complement* of normalisedDistance and
    // inverts, rather than raising normalisedDistance itself to the
    // exponent. A plain pow(normalisedDistance, exponent) with exponent > 1
    // is convex on [0, 1] (slow start, accelerating near 1) - a back-loaded
    // shape, the opposite of the brief's "most of the audible change
    // happens in the first third of the knob's travel, tapering off toward
    // 100%". This formulation is concave instead: a fast initial rise that
    // flattens out approaching 1, mirroring real proximity effect's
    // "accelerates then saturates" curve. See
    // CabConvolutionEngine::distanceLowShelfTaperExponent's doc comment for
    // the full rationale. The high-shelf is intentionally excluded from
    // this - it keeps the plain-linear taper it always had.
    float tapered (float normalisedDistance, float exponent) noexcept
    {
        return 1.0f - std::pow (1.0f - normalisedDistance, exponent);
    }

    // v0.3.0 ALLOCATION-FREE COEFFICIENT UPDATE (mandatory - see below).
    //
    // v0.2 used `*filter.state = *IIR::Coefficients<float>::make...(...)`,
    // which constructs a brand new REF-COUNTED Coefficients object - a heap
    // allocation - on the audio thread, on every engaged block. v0.3.0 makes
    // that strictly worse in two ways: coefficients are now recomputed every
    // 32 samples rather than once per block, and a 24 dB/oct slope has two
    // cascaded sections to update instead of one. At a 1024-sample block that
    // is 64 allocations per filter per block where there used to be one.
    //
    // ArrayCoefficients::make... returns a plain std::array BY VALUE (stack,
    // no heap), and assigning it into an EXISTING Coefficients object reuses
    // that object's already-allocated storage rather than replacing the
    // pointer. tests/AllocationGuardTests.cpp fails against the v0.2 idiom by
    // design, so this is not an optimisation - it is the prerequisite.
    //
    // The state pointer must be primed once (in prepare()) for the storage to
    // exist; every audio-thread update then writes into it.
    using ArrayCoefficients = juce::dsp::IIR::ArrayCoefficients<float>;
}

CabConvolutionEngine::CabConvolutionEngine() = default;

CabConvolutionEngine::~CabConvolutionEngine()
{
    // Stops the morph worker thread before any of the members it touches are
    // destroyed. Relying on MorphEngine's own destructor would be correct too,
    // but being explicit here documents that the engine owns a thread.
    morphEngine.releaseResources();
}

//==============================================================================
void CabConvolutionEngine::CutFilterChain::prepare (const juce::dsp::ProcessSpec& spec)
{
    twelve.prepare (spec);
    twentyFourA.prepare (spec);
    twentyFourB.prepare (spec);

    crossfadeSamplesRemaining = 0;
    previousSlope = activeSlope;
}

void CabConvolutionEngine::CutFilterChain::reset()
{
    twelve.reset();
    twentyFourA.reset();
    twentyFourB.reset();

    crossfadeSamplesRemaining = 0;
    previousSlope = activeSlope;
}

void CabConvolutionEngine::prepare (const juce::dsp::ProcessSpec& spec)
{
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

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

    morphEngine.prepare (spec);

    // Delay-line capacities. The two branch delays only ever need the IR B
    // Delay control's range; the Air delay needs the full time of flight. Both
    // get a couple of samples of headroom for the cubic interpolator's window.
    const auto millisecondsToSamples = [&] (float milliseconds)
    {
        return static_cast<int> (std::ceil (milliseconds * 0.001 * sampleRate)) + 4;
    };

    irBBranchDelay.prepare (sampleRate, numChannelsPrepared, millisecondsToSamples (irBDelayMaxMilliseconds));
    irABranchDelay.prepare (sampleRate, numChannelsPrepared, millisecondsToSamples (irBDelayMaxMilliseconds));
    distanceAirDelay.prepare (sampleRate, numChannelsPrepared, millisecondsToSamples (distanceAirMaxMilliseconds));

    // Not real-time safe (allocates) - fine here, prepare() is never called
    // from the audio thread. Never resized again in process().
    scratchBuffer.setSize (juce::jmax (1, numChannelsPrepared),
                            static_cast<int> (spec.maximumBlockSize),
                            false, false, true);
    slopeScratchBuffer.setSize (juce::jmax (1, numChannelsPrepared),
                                 static_cast<int> (spec.maximumBlockSize),
                                 false, false, true);
    morphScratchBuffer.setSize (juce::jmax (1, numChannelsPrepared),
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

    // v0.3.0 gain smoothers. Trim ramps over the standard 50 ms; polarity and
    // the blend-mode path crossfade have their own, shorter durations.
    irBTrimGainSmoothed.reset (sampleRate, smoothingTimeSeconds);
    irBTrimGainSmoothed.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (lastIrBTrimDb));

    irBPolarityGainSmoothed.reset (sampleRate, polarityCrossfadeSeconds);
    irBPolarityGainSmoothed.setCurrentAndTargetValue (lastIrBPolarityInverted ? -1.0f : 1.0f);

    morphMixSmoothed.reset (sampleRate, blendModeCrossfadeSeconds);
    morphMixSmoothed.setCurrentAndTargetValue (blendMode == BlendMode::Morph ? 1.0f : 0.0f);

    samplesSinceCoefficientUpdate = 0;

    reset();

    // Prime every filter's coefficient storage immediately. This does two
    // things: it means a subsequent engage starts from correct coefficients
    // rather than an uninitialised state, and - because the audio-thread
    // updates assign into these existing objects - it is what allocates the
    // coefficient storage once, here, off the audio thread.
    const auto loCutClamped = clampBelowNyquist (lastLoCutHz, sampleRate);
    const auto hiCutClamped = clampBelowNyquist (lastHiCutHz, sampleRate);

    *loCutFilter.twelve.state = ArrayCoefficients::makeHighPass (sampleRate, loCutClamped, filterQ);
    *loCutFilter.twentyFourA.state = ArrayCoefficients::makeHighPass (sampleRate, loCutClamped, butterworth4thOrderQ1);
    *loCutFilter.twentyFourB.state = ArrayCoefficients::makeHighPass (sampleRate, loCutClamped, butterworth4thOrderQ2);

    *hiCutFilter.twelve.state = ArrayCoefficients::makeLowPass (sampleRate, hiCutClamped, filterQ);
    *hiCutFilter.twentyFourA.state = ArrayCoefficients::makeLowPass (sampleRate, hiCutClamped, butterworth4thOrderQ1);
    *hiCutFilter.twentyFourB.state = ArrayCoefficients::makeLowPass (sampleRate, hiCutClamped, butterworth4thOrderQ2);

    const auto normalisedDistance = (lastDistancePercent - distanceMinPercent)
                                     / (distanceMaxPercent - distanceMinPercent);
    *distanceLowShelfFilter.state = ArrayCoefficients::makeLowShelf (
        sampleRate, distanceLowShelfFrequencyHz, filterQ,
        juce::Decibels::decibelsToGain (tapered (normalisedDistance, distanceLowShelfTaperExponent) * distanceLowShelfMaxCutDb));
    *distanceHighShelfFilter.state = ArrayCoefficients::makeHighShelf (
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
    morphEngine.reset();
    loCutFilter.reset();
    hiCutFilter.reset();
    distanceLowShelfFilter.reset();
    distanceHighShelfFilter.reset();
    outputLevel.reset();
    dryWetMixer.reset();

    irBBranchDelay.reset();
    irABranchDelay.reset();
    distanceAirDelay.reset();

    samplesSinceCoefficientUpdate = 0;
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

//==============================================================================
// v0.3.0 audio-thread-safe setters.

void CabConvolutionEngine::setBlendMode (BlendMode newMode) noexcept
{
    if (newMode == blendMode)
        return;

    blendMode = newMode;

    // A target change only; both paths keep running until the smoother
    // arrives, which is what makes the switch a crossfade rather than a cut.
    morphMixSmoothed.setTargetValue (newMode == BlendMode::Morph ? 1.0f : 0.0f);
}

void CabConvolutionEngine::setIrBTrimDb (float newTrimDb) noexcept
{
    lastIrBTrimDb = newTrimDb;
    irBTrimGainSmoothed.setTargetValue (juce::Decibels::decibelsToGain (newTrimDb));
}

void CabConvolutionEngine::setIrBPolarityInverted (bool shouldInvert) noexcept
{
    lastIrBPolarityInverted = shouldInvert;

    // Ramping through zero rather than jumping between +1 and -1: a hard flip
    // is a full-scale step discontinuity, i.e. the loudest click the plugin
    // could possibly make.
    irBPolarityGainSmoothed.setTargetValue (shouldInvert ? -1.0f : 1.0f);
}

void CabConvolutionEngine::setIrBDelayMs (float newDelayMs) noexcept
{
    lastIrBDelayMs = juce::jlimit (-irBDelayMaxMilliseconds, irBDelayMaxMilliseconds, newDelayMs);
}

void CabConvolutionEngine::setDistanceAirEnabled (bool shouldEnable) noexcept
{
    distanceAirEnabled = shouldEnable;
}

void CabConvolutionEngine::setLoCutSlope (Slope newSlope) noexcept
{
    if (newSlope == loCutFilter.activeSlope)
        return;

    loCutFilter.previousSlope = loCutFilter.activeSlope;
    loCutFilter.activeSlope = newSlope;
    loCutFilter.crossfadeSamplesRemaining = juce::jmax (1, static_cast<int> (slopeCrossfadeSeconds * sampleRate));
}

void CabConvolutionEngine::setHiCutSlope (Slope newSlope) noexcept
{
    if (newSlope == hiCutFilter.activeSlope)
        return;

    hiCutFilter.previousSlope = hiCutFilter.activeSlope;
    hiCutFilter.activeSlope = newSlope;
    hiCutFilter.crossfadeSamplesRemaining = juce::jmax (1, static_cast<int> (slopeCrossfadeSeconds * sampleRate));
}

void CabConvolutionEngine::setDuplicateFirstChannel (bool shouldDuplicate) noexcept
{
    duplicateFirstChannel = shouldDuplicate;
}

//==============================================================================
// v0.3.0 message-thread-only setters. Each reloads convolver content, which is
// a hard engine swap - see the click policy in the header.

void CabConvolutionEngine::setAlignMode (IrAlignment::Mode newMode)
{
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

    if (newMode == alignMode)
        return;

    alignMode = newMode;

    // Only IR B's alignment depends on this; IR A is the reference.
    if (hasUserIrB)
        applySlotB();
}

void CabConvolutionEngine::setGainMode (GainMode newMode)
{
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

    if (newMode == gainMode)
        return;

    gainMode = newMode;

    // Both slots are renormalised: the whole point of Loudness mode is that
    // the two are comparable to each other.
    applySlotA();
    applySlotB();
}

void CabConvolutionEngine::setIrAMinPhase (bool shouldApply)
{
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

    if (shouldApply == irAMinPhaseEnabled)
        return;

    irAMinPhaseEnabled = shouldApply;

    applySlotA();

    // IR A is IR B's alignment reference, and minimum-phasing it moved its
    // energy forward in time - so B's alignment against it is now stale.
    if (hasUserIrB)
        applySlotB();
}

void CabConvolutionEngine::setIrBMinPhase (bool shouldApply)
{
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

    if (shouldApply == irBMinPhaseEnabled)
        return;

    irBMinPhaseEnabled = shouldApply;

    applySlotB();
}

//==============================================================================
double CabConvolutionEngine::getTailLengthSeconds() const noexcept
{
    return juce::jmax (irALengthSeconds, irBLengthSeconds);
}

const juce::AudioBuffer<float>& CabConvolutionEngine::getRawImpulseResponse (int slotIndex) const noexcept
{
    return slotIndex == 0 ? lastIrARawBuffer : lastIrBRawBuffer;
}

double CabConvolutionEngine::getRawImpulseResponseSampleRate (int slotIndex) const noexcept
{
    return slotIndex == 0 ? lastIrARawSampleRate : lastIrBRawSampleRate;
}

bool CabConvolutionEngine::hasUserImpulseResponse (int slotIndex) const noexcept
{
    return slotIndex == 0 ? hasUserIrA : hasUserIrB;
}

// Applies the per-slot processing chain to a raw IR, in the order the
// processing actually has to happen:
//
//   min-phase transform (changes the waveform's timing)
//     -> loudness normalisation (measures the result, so must come after)
//
// Alignment is NOT done here - it is slot B's business only, and it needs the
// finished slot A as its reference, so applySlotB() does it.
juce::AudioBuffer<float> CabConvolutionEngine::prepareSlotForLoading (const juce::AudioBuffer<float>& raw,
                                                                       double rawSampleRate,
                                                                       bool applyMinPhase) const
{
    auto processed = applyMinPhase ? MinPhase::transform (raw) : raw;

    if (gainMode == GainMode::Loudness)
        processed = IrLoudness::applyLoudnessNormalisation (processed, rawSampleRate);

    return processed;
}

void CabConvolutionEngine::loadIntoConvolver (juce::dsp::Convolution& convolver,
                                               juce::AudioBuffer<float> buffer,
                                               double bufferSampleRate)
{
    const auto isStereo = (buffer.getNumChannels() >= 2 && numChannelsPrepared >= 2)
                               ? juce::dsp::Convolution::Stereo::yes
                               : juce::dsp::Convolution::Stereo::no;

    // In Loudness mode the buffer has ALREADY been scaled to the K-weighted
    // reference above, so JUCE must be told not to renormalise - doing both
    // would simply undo the perceptual weighting and leave plain energy
    // matching, i.e. Energy mode with extra steps.
    const auto normalise = gainMode == GainMode::Loudness
                                ? juce::dsp::Convolution::Normalise::no
                                : juce::dsp::Convolution::Normalise::yes;

    convolver.loadImpulseResponse (std::move (buffer),
                                    bufferSampleRate,
                                    isStereo,
                                    juce::dsp::Convolution::Trim::no,
                                    normalise);
}

void CabConvolutionEngine::applySlot (int slotIndex)
{
    if (slotIndex == 0)
        applySlotA();
    else
        applySlotB();
}

void CabConvolutionEngine::applySlotA()
{
    if (! hasUserIrA)
    {
        loadDefaultImpulseResponse();
        return;
    }

    auto processed = prepareSlotForLoading (lastIrARawBuffer, lastIrARawSampleRate, irAMinPhaseEnabled);

    // The alignment reference is the PROCESSED buffer, not the raw file: IR B
    // has to line up with what is actually playing, and min-phasing A moves
    // its energy forward in time.
    alignmentReferenceBuffer.makeCopyOf (processed);
    lastIrAOnsetSample = IrAlignment::detectOnsetSample (processed);
    lastIrASampleRate = lastIrARawSampleRate;

    irALengthSeconds = lastIrARawSampleRate > 0.0
                            ? static_cast<double> (processed.getNumSamples()) / lastIrARawSampleRate
                            : 0.0;

    morphEngine.setImpulseResponse (0, processed);

    loadIntoConvolver (convolution, std::move (processed), lastIrARawSampleRate);

    anyImpulseResponseLoaded = true;
}

void CabConvolutionEngine::applySlotB()
{
    if (! hasUserIrB)
    {
        loadDefaultImpulseResponseB();
        return;
    }

    auto processed = prepareSlotForLoading (lastIrBRawBuffer, lastIrBRawSampleRate, irBMinPhaseEnabled);

    // Inter-IR alignment, so blending the two never introduces comb-filtering
    // from a timing mismatch between their transients. In Precise mode this
    // also flips IR B's polarity when the two would otherwise partially
    // cancel (see IrAlignment.h).
    auto aligned = IrAlignment::alignToReference (processed,
                                                   lastIrBRawSampleRate,
                                                   alignmentReferenceBuffer,
                                                   lastIrAOnsetSample,
                                                   lastIrASampleRate,
                                                   alignMode);

    irBLengthSeconds = lastIrBRawSampleRate > 0.0
                            ? static_cast<double> (aligned.getNumSamples()) / lastIrBRawSampleRate
                            : 0.0;

    morphEngine.setImpulseResponse (1, aligned);

    loadIntoConvolver (convolutionB, std::move (aligned), lastIrBRawSampleRate);

    anyImpulseResponseBLoaded = true;
}

void CabConvolutionEngine::setImpulseResponse (juce::AudioBuffer<float> irBuffer, double irSampleRate)
{
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

    // The raw buffer is retained (v0.3.0): it is what the session embeds, what
    // a min-phase or gain-mode switch is re-derived from, and - being
    // pre-processing - what makes those switches non-destructive.
    lastIrARawBuffer.makeCopyOf (irBuffer);
    lastIrARawSampleRate = irSampleRate;
    hasUserIrA = true;

    applySlotA();

    // IR A's alignment reference just changed. If a real IR B is already
    // loaded, its alignment was computed against the *previous* reference and
    // is now stale - silently reintroducing comb-filtering the next time Blend
    // crosses back into an engaged range (see #13).
    if (irBNeedsAlignment)
        applySlotB();
}

void CabConvolutionEngine::loadDefaultImpulseResponse()
{
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

    const auto hadUserIr = hasUserIrA;

    hasUserIrA = false;
    lastIrARawBuffer.setSize (0, 0);

    // The delta IR's onset is trivially sample 0 - reset the phase-alignment
    // reference to match, at the engine's current sample rate.
    lastIrAOnsetSample = 0;
    lastIrASampleRate = sampleRate;
    irALengthSeconds = 0.0;

    alignmentReferenceBuffer.setSize (0, 0);

    // Normalise::no is essential here: normalising a unit impulse would
    // rescale it away from exact unity gain (JUCE's normalisation targets a
    // fixed reference energy, not "leave amplitude 1.0 alone"), which would
    // break the passthrough guarantee the default IR exists to provide. This
    // holds in Loudness mode too - the delta is the identity, not content to
    // be level-matched.
    convolution.loadImpulseResponse (makeDeltaImpulseResponse(),
                                      sampleRate,
                                      juce::dsp::Convolution::Stereo::no,
                                      juce::dsp::Convolution::Trim::no,
                                      juce::dsp::Convolution::Normalise::no);

    {
        juce::AudioBuffer<float> delta (1, 1);
        delta.setSample (0, 0, 1.0f);
        morphEngine.setImpulseResponse (0, delta);
    }

    anyImpulseResponseLoaded = true;

    // Same rationale as the end of setImpulseResponse() above: this changed
    // the alignment reference, so an already-loaded real IR B must be
    // re-aligned against it rather than left pointing at a stale onset (#13).
    // Guarded on hadUserIr so the prepare()-time priming call does not
    // recursively reload slot B before it exists.
    if (irBNeedsAlignment && hadUserIr)
        applySlotB();
}

void CabConvolutionEngine::setImpulseResponseB (juce::AudioBuffer<float> irBuffer, double irSampleRate)
{
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

    // Retain the raw, pre-processing buffer so a later IR A reload (or a
    // min-phase/gain-mode switch) can re-derive slot B without the caller
    // having to reload the file - see applySlotB() and #13.
    lastIrBRawBuffer.makeCopyOf (irBuffer);
    lastIrBRawSampleRate = irSampleRate;
    irBNeedsAlignment = true;
    hasUserIrB = true;

    applySlotB();
}

void CabConvolutionEngine::loadDefaultImpulseResponseB()
{
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

    hasUserIrB = false;
    irBLengthSeconds = 0.0;

    convolutionB.loadImpulseResponse (makeDeltaImpulseResponse(),
                                       sampleRate,
                                       juce::dsp::Convolution::Stereo::no,
                                       juce::dsp::Convolution::Trim::no,
                                       juce::dsp::Convolution::Normalise::no);

    {
        juce::AudioBuffer<float> delta (1, 1);
        delta.setSample (0, 0, 1.0f);
        morphEngine.setImpulseResponse (1, delta);
    }

    anyImpulseResponseBLoaded = true;

    // The default delta IR has no meaningful onset to keep aligned - clear
    // the retained-raw-buffer bookkeeping so a subsequent IR A reload
    // doesn't try to re-align it (see setImpulseResponse() above and #13).
    irBNeedsAlignment = false;
    lastIrBRawBuffer.setSize (0, 0);
}

void CabConvolutionEngine::updateCutCoefficients (bool loCutBypassed, bool hiCutBypassed,
                                                   float loCutHz, float hiCutHz) noexcept
{
    // All four assignments below write into Coefficients objects that already
    // exist (primed in prepare()), from stack-allocated std::arrays - no heap
    // traffic on the audio thread. See the ArrayCoefficients note at the top
    // of this file for why the v0.2 idiom had to go.
    if (! loCutBypassed)
    {
        const auto clamped = clampBelowNyquist (loCutHz, sampleRate);

        if (loCutFilter.activeSlope == Slope::TwelveDbPerOctave
            || loCutFilter.crossfadeSamplesRemaining > 0)
            *loCutFilter.twelve.state = ArrayCoefficients::makeHighPass (sampleRate, clamped, filterQ);

        if (loCutFilter.activeSlope == Slope::TwentyFourDbPerOctave
            || loCutFilter.crossfadeSamplesRemaining > 0)
        {
            *loCutFilter.twentyFourA.state = ArrayCoefficients::makeHighPass (sampleRate, clamped, butterworth4thOrderQ1);
            *loCutFilter.twentyFourB.state = ArrayCoefficients::makeHighPass (sampleRate, clamped, butterworth4thOrderQ2);
        }
    }

    if (! hiCutBypassed)
    {
        const auto clamped = clampBelowNyquist (hiCutHz, sampleRate);

        if (hiCutFilter.activeSlope == Slope::TwelveDbPerOctave
            || hiCutFilter.crossfadeSamplesRemaining > 0)
            *hiCutFilter.twelve.state = ArrayCoefficients::makeLowPass (sampleRate, clamped, filterQ);

        if (hiCutFilter.activeSlope == Slope::TwentyFourDbPerOctave
            || hiCutFilter.crossfadeSamplesRemaining > 0)
        {
            *hiCutFilter.twentyFourA.state = ArrayCoefficients::makeLowPass (sampleRate, clamped, butterworth4thOrderQ1);
            *hiCutFilter.twentyFourB.state = ArrayCoefficients::makeLowPass (sampleRate, clamped, butterworth4thOrderQ2);
        }
    }
}

void CabConvolutionEngine::updateDistanceCoefficients (float distancePercent) noexcept
{
    const auto normalisedDistance = (distancePercent - distanceMinPercent)
                                     / (distanceMaxPercent - distanceMinPercent);

    *distanceLowShelfFilter.state = ArrayCoefficients::makeLowShelf (
        sampleRate, distanceLowShelfFrequencyHz, filterQ,
        juce::Decibels::decibelsToGain (tapered (normalisedDistance, distanceLowShelfTaperExponent) * distanceLowShelfMaxCutDb));
    *distanceHighShelfFilter.state = ArrayCoefficients::makeHighShelf (
        sampleRate, distanceHighShelfFrequencyHz, filterQ,
        juce::Decibels::decibelsToGain (normalisedDistance * distanceHighShelfMaxCutDb));
}

void CabConvolutionEngine::processCutFilter (CutFilterChain& chain,
                                              juce::dsp::AudioBlock<float>& block,
                                              int numSamples) noexcept
{
    const auto runSlope = [] (CutFilterChain& target, Slope slope, juce::dsp::AudioBlock<float>& destination)
    {
        juce::dsp::ProcessContextReplacing<float> context (destination);

        if (slope == Slope::TwelveDbPerOctave)
        {
            target.twelve.process (context);
        }
        else
        {
            // Two cascaded 2nd-order sections with the Butterworth Q pair make
            // a 4th-order Butterworth: -3 dB at the corner, 24 dB/oct beyond.
            target.twentyFourA.process (context);
            target.twentyFourB.process (context);
        }
    };

    if (chain.crossfadeSamplesRemaining <= 0)
    {
        runSlope (chain, chain.activeSlope, block);
        return;
    }

    // Mid slope change: the two chains have different IIR states and different
    // coefficients, so switching outright would jump the filter's memory - an
    // audible click. Running both and crossfading their outputs is the only
    // way to get from one response to the other continuously.
    const auto fadeLength = juce::jmax (1, static_cast<int> (slopeCrossfadeSeconds * sampleRate));

    juce::dsp::AudioBlock<float> scratchBlock (slopeScratchBuffer);
    auto scratchSub = scratchBlock.getSubBlock (0, static_cast<size_t> (numSamples));
    scratchSub.copyFrom (block);

    runSlope (chain, chain.previousSlope, block);
    runSlope (chain, chain.activeSlope, scratchSub);

    for (int sample = 0; sample < numSamples; ++sample)
    {
        const auto remaining = chain.crossfadeSamplesRemaining - sample;

        // Amplitude-complementary, as everywhere else in this engine: the two
        // filter outputs are highly correlated (same signal, similar response),
        // so equal-power gains would bulge in the middle.
        const auto gainNew = remaining > 0
                                  ? 1.0f - static_cast<float> (remaining) / static_cast<float> (fadeLength)
                                  : 1.0f;

        for (size_t channel = 0; channel < block.getNumChannels(); ++channel)
        {
            auto* out = block.getChannelPointer (channel);
            const auto* fresh = scratchSub.getChannelPointer (channel);

            out[sample] = out[sample] * (1.0f - gainNew) + fresh[sample] * gainNew;
        }
    }

    chain.crossfadeSamplesRemaining = juce::jmax (0, chain.crossfadeSamplesRemaining - numSamples);

    if (chain.crossfadeSamplesRemaining == 0)
        chain.previousSlope = chain.activeSlope;
}

void CabConvolutionEngine::process (juce::dsp::AudioBlock<float>& block)
{
    const auto numSamples = block.getNumSamples();

    if (numSamples == 0)
        return;

    const auto numSamplesInt = static_cast<int> (numSamples);
    const auto numChannels = block.getNumChannels();

    // Mono-in/stereo-out: fill the second channel from the first at the very
    // top, so a mono DI drives the whole chain in stereo rather than playing
    // out of one side (survey gap #10).
    if (duplicateFirstChannel && numChannels >= 2)
        for (size_t channel = 1; channel < numChannels; ++channel)
            juce::FloatVectorOperations::copy (block.getChannelPointer (channel),
                                                block.getChannelPointer (0),
                                                numSamplesInt);

    const auto wetMix = mixSmoothed.skip (numSamplesInt);

    // Bypass decisions are taken once per block from BOTH ends of each
    // smoother's current ramp: a filter counts as engaged if either its
    // current or its target value is engaged. For a static setting (the case
    // the passthrough null tests pin) both ends are equal and this is exactly
    // v0.2's decision; during a ramp it errs toward engaged, which avoids the
    // filter flicking on and off around the epsilon.
    const auto loCutEngagedAtEither = loCutFrequencySmoothed.getCurrentValue() > loCutMinHz + bypassEpsilonHz
                                       || loCutFrequencySmoothed.getTargetValue() > loCutMinHz + bypassEpsilonHz;
    const auto hiCutEngagedAtEither = hiCutFrequencySmoothed.getCurrentValue() < hiCutMaxHz - bypassEpsilonHz
                                       || hiCutFrequencySmoothed.getTargetValue() < hiCutMaxHz - bypassEpsilonHz;
    const auto distanceEngagedAtEither = distanceSmoothed.getCurrentValue() > distanceMinPercent + distanceBypassEpsilonPercent
                                          || distanceSmoothed.getTargetValue() > distanceMinPercent + distanceBypassEpsilonPercent;

    const bool loCutBypassed = ! loCutEngagedAtEither;
    const bool hiCutBypassed = ! hiCutEngagedAtEither;
    const bool distanceBypassed = ! distanceEngagedAtEither;

    // Defensive fallback: the scratch buffers are sized to maximumBlockSize in
    // prepare(), so a host that (against its own promise) sends a larger block
    // here would overrun them. Rather than risk that, Blend is treated as
    // disengaged for that one block (falls back to IR A only) - safer than
    // allocating or writing out of bounds on the audio thread.
    const bool scratchLargeEnough = numSamples <= static_cast<size_t> (scratchBuffer.getNumSamples())
                                     && numSamples <= static_cast<size_t> (morphScratchBuffer.getNumSamples())
                                     && numSamples <= static_cast<size_t> (slopeScratchBuffer.getNumSamples());

    const bool blendEngaged = (blendSmoothed.getCurrentValue() > blendBypassEpsilon
                                || blendSmoothed.getTargetValue() > blendBypassEpsilon)
                               && scratchLargeEnough;

    // Which convolution paths must run this block. Both run only while a
    // blend-mode change is crossfading between them.
    const auto morphMixCurrent = morphMixSmoothed.getCurrentValue();
    const auto morphMixTarget = morphMixSmoothed.getTargetValue();

    const bool morphPathActive = (morphMixCurrent > 0.0f || morphMixTarget > 0.0f)
                                  && morphEngine.isReady() && scratchLargeEnough;
    const bool stockPathActive = ! morphPathActive
                                  || morphMixCurrent < 1.0f
                                  || morphMixTarget < 1.0f;

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

    dryWetMixer.setWetMixProportion (wetMix);

    // Capture the pre-processing signal as "dry" before convolution or
    // filtering touches `block`. DryWetMixer internally delays this by
    // getLatencySamples() (set via setWetLatency in prepare()) so it stays
    // time-aligned with the wet path below, whatever that latency is.
    dryWetMixer.pushDrySamples (block);

    //==========================================================================
    // Convolution stage.

    const bool needMorphScratch = morphPathActive && stockPathActive;

    if (needMorphScratch)
    {
        juce::dsp::AudioBlock<float> morphBlock (morphScratchBuffer);
        auto morphSub = morphBlock.getSubBlock (0, numSamples);
        morphSub.copyFrom (block);
    }

    if (stockPathActive)
    {
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

        juce::dsp::ProcessContextReplacing<float> contextA (block);
        convolution.process (contextA);

        if (blendEngaged)
        {
            juce::dsp::AudioBlock<float> scratchBlock (scratchBuffer);
            auto scratchSub = scratchBlock.getSubBlock (0, numSamples);

            juce::dsp::ProcessContextReplacing<float> contextB (scratchSub);
            convolutionB.process (contextB);

            // Dual-sided branch delay (see setIrBDelayMs). Both delay times are
            // >= 0 and both processors are skipped entirely when they are zero,
            // which is what keeps the neutral setting bit-identical to v0.2.
            // The "or still non-zero" clause lets a delay that has just been
            // returned to zero glide down instead of being cut off mid-ramp.
            const auto delaySamples = std::abs (lastIrBDelayMs) * 0.001f * static_cast<float> (sampleRate);

            const auto bDelay = lastIrBDelayMs > 0.0f ? delaySamples : 0.0f;
            const auto aDelay = lastIrBDelayMs < 0.0f ? delaySamples : 0.0f;

            if (bDelay > 0.0f || irBBranchDelay.getCurrentDelaySamples() > 0.0f)
            {
                irBBranchDelay.setDelaySamples (bDelay);
                irBBranchDelay.process (scratchSub);
            }

            if (aDelay > 0.0f || irABranchDelay.getCurrentDelaySamples() > 0.0f)
            {
                irABranchDelay.setDelaySamples (aDelay);
                irABranchDelay.process (block);
            }

            // Per-sample gains (survey gap #8): Blend, Trim and Polarity each
            // advance one step per sample rather than being stepped once per
            // block, so a fast move ramps smoothly instead of stepping at block
            // boundaries. At a static setting every smoother sits on its target
            // and this is arithmetically identical to v0.2's constant gain.
            for (size_t sample = 0; sample < numSamples; ++sample)
            {
                const auto blendProportion = blendSmoothed.getNextValue();
                const auto trimGain = irBTrimGainSmoothed.getNextValue();
                const auto polarityGain = irBPolarityGainSmoothed.getNextValue();

                const auto branchGain = trimGain * polarityGain * blendProportion;
                const auto aGain = 1.0f - blendProportion;

                for (size_t channel = 0; channel < numChannels; ++channel)
                {
                    auto* a = block.getChannelPointer (channel);
                    const auto* b = scratchSub.getChannelPointer (channel);

                    a[sample] = a[sample] * aGain + b[sample] * branchGain;
                }
            }
        }
        else
        {
            // Keep every smoother advancing at the same rate whether or not the
            // branch runs, so engaging Blend never resumes from a stale ramp
            // position.
            blendSmoothed.skip (numSamplesInt);
            irBTrimGainSmoothed.skip (numSamplesInt);
            irBPolarityGainSmoothed.skip (numSamplesInt);
        }
    }
    else
    {
        blendSmoothed.skip (numSamplesInt);
        irBTrimGainSmoothed.skip (numSamplesInt);
        irBPolarityGainSmoothed.skip (numSamplesInt);
    }

    if (morphPathActive)
    {
        if (needMorphScratch)
        {
            juce::dsp::AudioBlock<float> morphBlock (morphScratchBuffer);
            auto morphSub = morphBlock.getSubBlock (0, numSamples);

            morphEngine.process (morphSub);

            // Crossfade the two PATH OUTPUTS. Nothing about this implies a
            // crossfade inside either convolver - both simply run, and their
            // results are blended.
            for (size_t sample = 0; sample < numSamples; ++sample)
            {
                const auto morphGain = morphMixSmoothed.getNextValue();
                const auto stockGain = 1.0f - morphGain;

                for (size_t channel = 0; channel < numChannels; ++channel)
                {
                    auto* out = block.getChannelPointer (channel);
                    const auto* morphed = morphSub.getChannelPointer (channel);

                    out[sample] = out[sample] * stockGain + morphed[sample] * morphGain;
                }
            }
        }
        else
        {
            morphEngine.process (block);
            morphMixSmoothed.skip (numSamplesInt);
        }
    }
    else
    {
        morphMixSmoothed.skip (numSamplesInt);
    }

    //==========================================================================
    // Distance Air: the time-of-flight component of mic distance, applied to
    // the wet path before the Distance shelves' tonal model. Skipped entirely
    // when off or at Distance 0%, so it cannot affect a v0.2 session.
    const auto airTargetSamples = (distanceAirEnabled && ! distanceBypassed)
                                       ? (distanceSmoothed.getTargetValue() - distanceMinPercent)
                                             / (distanceMaxPercent - distanceMinPercent)
                                             * distanceAirMaxMilliseconds * 0.001f
                                             * static_cast<float> (sampleRate)
                                       : 0.0f;

    if (airTargetSamples > 0.0f || distanceAirDelay.getCurrentDelaySamples() > 0.0f)
    {
        distanceAirDelay.setDelaySamples (airTargetSamples);
        distanceAirDelay.process (block);
    }

    //==========================================================================
    // Filter stage, in coefficientUpdateInterval-sized sub-blocks so a fast
    // sweep steps every 32 samples instead of once per (up to 1024-sample)
    // block. Static settings are unaffected: every sub-block computes the same
    // coefficients the single per-block update used to.
    size_t offset = 0;

    while (offset < numSamples)
    {
        const auto chunk = juce::jmin (static_cast<size_t> (coefficientUpdateInterval),
                                        numSamples - offset);
        const auto chunkInt = static_cast<int> (chunk);

        auto sub = block.getSubBlock (offset, chunk);

        const auto loCutHz = loCutFrequencySmoothed.skip (chunkInt);
        const auto hiCutHz = hiCutFrequencySmoothed.skip (chunkInt);
        const auto distancePercent = distanceSmoothed.skip (chunkInt);

        updateCutCoefficients (loCutBypassed, hiCutBypassed, loCutHz, hiCutHz);

        if (! distanceBypassed)
        {
            updateDistanceCoefficients (distancePercent);

            juce::dsp::ProcessContextReplacing<float> distanceContext (sub);
            distanceLowShelfFilter.process (distanceContext);
            distanceHighShelfFilter.process (distanceContext);
        }

        if (! loCutBypassed)
            processCutFilter (loCutFilter, sub, chunkInt);

        if (! hiCutBypassed)
            processCutFilter (hiCutFilter, sub, chunkInt);

        offset += chunk;
    }

    dryWetMixer.mixWetSamples (block);

    juce::dsp::ProcessContextReplacing<float> outputContext (block);
    outputLevel.process (outputContext);
}
