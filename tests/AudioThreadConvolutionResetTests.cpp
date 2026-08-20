#include "dsp/CabConvolutionEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

// Regression coverage for #34: pluginval --strictness-level 10 aborted with
//
//     libc++abi: terminating due to uncaught exception of type
//     std::__1::bad_function_call: std::exception
//     pluginval received Abort trap: 6, exiting immediately
//
// during its "Restoring default layout" phase. Same exception class as #27,
// but a different producer - and one that #27's fix structurally cannot
// cover.
//
// ROOT CAUSE. Until this fix, CabConvolutionEngine::process() cleared slot
// B's convolver on the AUDIO thread whenever Blend crossed from disengaged
// to engaged (introduced in #18, for #12's stale-overlap-add-tail problem):
//
//     if (blendEngaged && ! blendEngagedPreviously)
//         convolutionB.reset();
//
// juce::dsp::Convolution::reset() is not a passive state clear. In JUCE
// 8.0.14 (modules/juce_dsp/frequency/juce_Convolution.cpp) it is a queue
// PRODUCER:
//
//     Convolution::reset()            -> Impl::reset()
//     Impl::reset()                   -> destroyPreviousEngine()
//     destroyPreviousEngine()         -> messageQueue->pimpl->push (command)
//
// and destroyPreviousEngine() pushes unconditionally - it builds the
// FixedSizeFunction<400, void()> and pushes it even when there is no
// previous engine to destroy. BackgroundMessageQueue::push() carries an
// explicit contract in that same file:
//
//     // Push functions here, and they'll be called later on a background thread.
//     // This function is wait-free.
//     // This function is only safe to call from a single thread at a time.
//     bool push (IncomingCommand& command) { return queue.push (command); }
//
// It wraps Queue<IncomingCommand>, i.e. an AbstractFifo - single producer.
// The class-level documentation in juce_Convolution.h is stricter still:
//
//     Threading: It is not safe to interleave calls to the methods of this
//     class. If you need to load new impulse responses during processing the
//     load() calls must be synchronised with process() calls [...]
//
// Meanwhile the message thread pushes into the SAME per-instance queue from
// every slot-B loader path - setImpulseResponseB(), setGainMode(),
// setIrBMinPhase(), setAlignMode(), prepare() - each of which ends in
// Convolution::loadImpulseResponse() -> ConvolutionEngineQueue::callLater()
// -> postPendingCommand() -> push().
//
// Two concurrent producers on a single-producer AbstractFifo means two
// overlapping fifo.write(1) reservations: a slot can be published before it
// has been written. JUCE's convolution background loader thread then pops a
// default-constructed FixedSizeFunction and invokes it, which throws
// std::bad_function_call on a thread with no reachable catch handler, and
// the process aborts. That is exactly the CI signature above; pluginval's
// layout-restore phase automates parameters (Blend included) while
// re-preparing the processor, which is precisely this interleaving.
//
// #27's messageThreadMutex cannot help here: the audio thread must never
// take that mutex, so serialising the message-thread entry points against
// each other leaves the audio-thread producer completely uncovered.
//
// THE FIX (src/dsp/CabConvolutionEngine.{h,cpp}) has two halves, because
// writing this test first turned up a second producer pair as well.
//
//   1. process() no longer resets slot B's convolver. Instead the engine
//      keeps feeding it SILENCE for the length of its impulse response after
//      the IR B branch goes inactive ("silent flush"). A convolver is a
//      finite-memory LTI system: once irSize zero samples have been pushed
//      through it, its output can no longer contain any contribution from
//      pre-disengagement input - which is the exact guarantee #12 needed.
//      Same argument applied to CabConvolutionEngine::reset(), which called
//      Convolution::reset() on both slots and is now serialised behind
//      messageThreadMutex with the rest of the host-callback group.
//
//   2. IR loads moved off the message thread and onto the audio thread. With
//      (1) in place this test still crashed roughly once in sixty runs,
//      because Convolution::processSamples() ALSO calls
//      ConvolutionEngineQueue::postPendingCommand(): a load issued from the
//      message thread leaves a window between the shared `pendingCommand`
//      slot being written and being pushed, in which both threads push. That
//      window predates #18 and is inherent to loading from a non-audio
//      thread, which is exactly why juce_Convolution.h says the load "must
//      be synchronised with process() calls, which in practice means making
//      the load() call from the audio thread". The message thread now stages
//      the finished buffer behind a SpinLock and process() performs the
//      wait-free loadImpulseResponse() itself.
//
// Together: while audio is running, the audio thread is the only thread that
// touches a live convolver, so there is only ever one producer.
//
// THIS TEST reproduces the two-producer interleaving directly: one thread
// stands in for the audio thread and drives Blend through repeated
// disengaged -> engaged transitions, while a second thread hammers the
// message-thread slot-B loader paths. Being a genuine data race this is a
// best-effort reproduction, not a guarantee - see the PR description for the
// red-verification evidence against the pre-fix code. The actual safety
// guarantee is the single-producer invariant above; this test is the
// trip-wire against it being undone.
namespace
{
    // Deliberately low: CabConvolutionEngine's parameter smoothing runs for
    // smoothingTimeSeconds (50 ms), which at 8 kHz is 400 samples and
    // therefore settles completely inside a single 512-sample block. That
    // makes every loop iteration below perform exactly one disengaged ->
    // engaged transition in three process() calls, which is what gives this
    // test its transition rate. The race itself is sample-rate independent.
    constexpr double raceSampleRate = 8000.0;
    constexpr int raceBlockSize = 512;

    // Wall-clock budget rather than a fixed iteration count, so the test
    // costs the same on a fast and a slow runner.
    constexpr auto raceBudget = std::chrono::seconds (6);

    juce::AudioBuffer<float> makeDecayingIr (int numSamples)
    {
        juce::AudioBuffer<float> ir (1, numSamples);

        for (int i = 0; i < numSamples; ++i)
            ir.setSample (0, i, static_cast<float> (std::sin (i * 0.05) * std::exp (-i / 40.0)));

        return ir;
    }
}

TEST_CASE ("Audio-thread Blend re-engagement never races the message thread's slot-B loader",
           "[dsp][engine][threading][blend]")
{
    CabConvolutionEngine engine;
    engine.setMixProportion (1.0f);
    engine.setLevelDb (0.0f);
    engine.setBlendProportion (0.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = raceSampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (raceBlockSize);
    spec.numChannels = 2;

    engine.prepare (spec);

    const auto irTemplate = makeDecayingIr (128);

    // A real IR in slot B up front, so the loader thread below is exercising
    // the full alignment/normalisation/load path rather than the cheap
    // default-delta shortcut, and so the convolver actually owns an engine
    // that reset() would have had a previous copy of to destroy.
    {
        juce::AudioBuffer<float> initial;
        initial.makeCopyOf (irTemplate);
        engine.setImpulseResponseB (std::move (initial), raceSampleRate);
        engine.prepare (spec); // drain the async load so slot B is live
    }

    std::atomic<bool> stop { false };
    std::atomic<bool> sawNonFiniteOutput { false };
    std::atomic<int> transitions { 0 };
    std::atomic<int> loads { 0 };

    // Stands in for the message thread: every one of these calls ends in
    // juce::dsp::Convolution::loadImpulseResponse() on slot B and therefore
    // in a BackgroundMessageQueue::push(). They are serialised against each
    // other by #27's messageThreadMutex - which is exactly why they are the
    // *other* producer, not the same one.
    std::thread loaderThread ([&]
    {
        int i = 0;

        while (! stop.load (std::memory_order_relaxed))
        {
            juce::AudioBuffer<float> ir;
            ir.makeCopyOf (irTemplate);
            engine.setImpulseResponseB (std::move (ir), raceSampleRate);

            // Two more message-thread paths into the same queue, so the test
            // is not narrowly tied to one loader entry point.
            engine.setGainMode ((i % 2 == 0) ? CabConvolutionEngine::GainMode::Energy
                                             : CabConvolutionEngine::GainMode::Loudness);
            engine.setIrBMinPhase (i % 2 == 0);

            loads.fetch_add (1, std::memory_order_relaxed);
            ++i;
        }
    });

    // Stands in for the audio thread. Three process() calls per iteration:
    //
    //   1. Blend target 1 with the smoother still at 0 -> blendEngaged flips
    //      false -> true. This is the block that used to call
    //      convolutionB.reset() from here.
    //   2. Blend target 0 with the smoother at 1 -> still engaged, ramps
    //      down to 0 within this block (see raceSampleRate above).
    //   3. Smoother and target both at 0 -> disengaged, so the next
    //      iteration's first block is another false -> true transition.
    juce::AudioBuffer<float> buffer (2, raceBlockSize);
    const auto deadline = std::chrono::steady_clock::now() + raceBudget;

    while (std::chrono::steady_clock::now() < deadline)
    {
        for (int step = 0; step < 3; ++step)
        {
            if (step == 0)
                engine.setBlendProportion (1.0f);
            else if (step == 1)
                engine.setBlendProportion (0.0f);

            TestHelpers::fillWithSine (buffer, raceSampleRate, 220.0, 0.5f);

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);

            if (! TestHelpers::allSamplesFinite (buffer))
                sawNonFiniteOutput.store (true, std::memory_order_relaxed);
        }

        transitions.fetch_add (1, std::memory_order_relaxed);
    }

    stop.store (true, std::memory_order_relaxed);
    loaderThread.join();

    // Guards against the test silently degenerating into something that
    // never exercises the interleaving it is named after.
    INFO ("Blend disengaged->engaged transitions: " << transitions.load()
          << ", slot-B loads: " << loads.load());
    CHECK (transitions.load() > 100);
    CHECK (loads.load() > 10);

    CHECK_FALSE (sawNonFiniteOutput.load());
}
