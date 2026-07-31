#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_events/juce_events.h>

#include <atomic>
#include <cmath>
#include <thread>

// Regression coverage for #27: pluginval hit
// "libc++abi: terminating due to uncaught exception of type
// std::__1::bad_function_call" during its Automation test at 96 kHz (once at
// block size 64, once at block size 1024, on an otherwise-unmodified v0.3.0
// build - reproduced twice on a docs-only PR, so the defect was already on
// main).
//
// ROOT CAUSE. NaveAudioProcessor::reconfigureEngineFromParameters() - which
// calls CabConvolutionEngine's four "message-thread only" setters
// (setAlignMode/setGainMode/setIrAMinPhase/setIrBMinPhase), each of which can
// reload a juce::dsp::Convolution slot via applySlotA()/applySlotB() - has
// TWO unsynchronised entry points:
//
//   1. Directly from NaveAudioProcessor::prepareToPlay() (and
//      setStateInformation()), called by the host on whatever thread the
//      host chooses - the VST3/AU contract guarantees only that it is not
//      the audio thread, NOT that it is JUCE's own MessageManager thread.
//   2. Indirectly via AsyncUpdater::handleAsyncUpdate(), which always runs on
//      the real JUCE message thread, triggered whenever host automation of
//      alignMode/irGainMode/irAMinPhase/irBMinPhase arrives (typically on the
//      audio thread, per PluginProcessor.h's own class docs) and calls
//      triggerAsyncUpdate().
//
// When a host's prepareToPlay()-calling thread differs from JUCE's message
// thread - true of pluginval, which drives its test sequence from its own
// thread while the JUCE message thread runs independently for GUI support -
// both paths can call into CabConvolutionEngine concurrently. Both can reach
// juce::dsp::Convolution::loadImpulseResponse(), whose background hand-off
// (juce::dsp::BackgroundMessageQueue::push(), in JUCE 8.0.14's
// juce_Convolution.cpp) is documented "only safe to call from a single
// thread at a time" - concurrent, unsynchronised callers can corrupt its
// internal FixedSizeFunction command slots, and the background convolution
// loader thread later invokes one that is empty/corrupted, throwing
// std::bad_function_call on a thread with no reachable catch handler, so the
// whole process aborts (libc++abi's "terminating due to uncaught exception").
//
// 96 kHz is the trigger, not the cause: prepare() and applySlotA()/
// applySlotB() (MinPhase transform, loudness analysis, IR resampling) all
// take measurably longer at 96 kHz than at 44.1/48 kHz, which widens the
// race window enough to hit in practice - consistent with #27's own
// observation that the crash is "timing/order dependent" (it did not
// reproduce on the v0.3.0 release CI run, and reproduced at two different
// block sizes across two CI runs).
//
// THE FIX (src/dsp/CabConvolutionEngine.{h,cpp}) serialises every
// "message-thread only" method behind a std::recursive_mutex
// (messageThreadMutex), so the two entry points above can never touch the
// convolution engines concurrently regardless of which OS threads they land
// on - this removes the race structurally, not just makes it less likely.
// The mutex is never taken by any audio-thread method (reset()/process()/the
// audio-thread-safe setters), so it adds no lock/allocation on the audio
// thread and is not a behavioural change to audio processing.
//
// THIS TEST reproduces the concurrent-entry scenario directly: one thread
// repeatedly reprepares at 96 kHz with both a small (64) and a large (1024)
// block size and processes audio (simulating the host's own thread), while a
// second thread "automates" the four message-thread-only parameters via
// setValueNotifyingHost() (simulating audio-thread-delivered host
// automation), while the real JUCE message thread (this test's own calling
// thread) pumps its dispatch loop so the resulting async updates actually
// fire concurrently with the reprepare/process cycle - exactly the
// concurrency pluginval's environment provides and that a purely
// single-threaded test cannot exercise.
//
// Being a genuine data race, this is a best-effort reproduction rather than
// a guarantee: during development it reliably aborted the unfixed binary
// with the exact reported std::bad_function_call within a couple of dozen
// repeated runs (see the PR description for the full red-verification
// evidence) but any single run could still get lucky and not hit the window.
// The actual safety guarantee is the mutex in CabConvolutionEngine, which
// makes the race structurally impossible; this test exists as a trip-wire
// against that mutex being removed or bypassed in future.
TEST_CASE ("Concurrent prepareToPlay and automation-driven reconfigure survive a 96k reprepare", "[processor][threading][v030]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto* alignParam = processor.apvts.getParameter (ParamIDs::alignMode);
    auto* gainModeParam = processor.apvts.getParameter (ParamIDs::irGainMode);
    auto* minPhaseAParam = processor.apvts.getParameter (ParamIDs::irAMinPhase);
    auto* minPhaseBParam = processor.apvts.getParameter (ParamIDs::irBMinPhase);

    REQUIRE (alignParam != nullptr);
    REQUIRE (gainModeParam != nullptr);
    REQUIRE (minPhaseAParam != nullptr);
    REQUIRE (minPhaseBParam != nullptr);

    // A real (non-trivial) IR in both slots, so the message-thread-only
    // setters below do real work (MinPhase transform + loudness analysis)
    // rather than the cheap default-delta path - this is what widens the
    // race window at 96k in the first place (see the comment above).
    {
        juce::AudioBuffer<float> ir (1, 2048);
        for (int i = 0; i < ir.getNumSamples(); ++i)
            ir.setSample (0, i, static_cast<float> (std::sin (i * 0.01) * std::exp (-i / 1000.0)));

        const auto irFile = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                 .getChildFile ("nave-issue27-regression-ir.wav");
        REQUIRE (TestHelpers::writeWavFile (irFile, ir, 48000.0));
        REQUIRE (processor.loadImpulseResponseFromFile (irFile));
        REQUIRE (processor.loadImpulseResponseFromFileB (irFile));
        irFile.deleteFile();
    }

    std::atomic<bool> stop { false };
    std::atomic<bool> sawNonFiniteOutput { false };

    // Simulates host automation of the four message-thread-only parameters,
    // delivered from a non-message thread (real DAWs typically deliver
    // automation from the audio thread). Each setValueNotifyingHost() call
    // synchronously invokes NaveAudioProcessor::parameterChanged() ->
    // triggerAsyncUpdate() on THIS thread; the actual reconfiguration is
    // dispatched later, on the real message thread, by the dispatch-loop
    // pump below.
    std::thread automationThread ([&]
    {
        int i = 0;
        while (! stop.load (std::memory_order_relaxed))
        {
            const float v = (i++ % 2 == 0) ? 1.0f : 0.0f;
            alignParam->setValueNotifyingHost (v);
            gainModeParam->setValueNotifyingHost (v);
            minPhaseAParam->setValueNotifyingHost (v);
            minPhaseBParam->setValueNotifyingHost (v);
            std::this_thread::yield();
        }
    });

    // Simulates the host's own prepareToPlay()-calling thread, which per the
    // VST3/AU specs is not guaranteed to be JUCE's message thread: repeatedly
    // reprepares at 96k with both a small and a large block size and
    // processes blocks, mirroring pluginval's Audio processing/Automation
    // test sweep (see #27). Catch2's assertion machinery is not meant to be
    // driven from a non-test thread, so this records failures into a plain
    // atomic instead of calling REQUIRE() directly.
    std::thread hostThread ([&]
    {
        for (int iteration = 0; iteration < 40; ++iteration)
        {
            for (int blockSize : { 64, 1024 })
            {
                processor.prepareToPlay (96000.0, blockSize);

                juce::AudioBuffer<float> buffer (2, blockSize);
                juce::MidiBuffer midi;

                for (int block = 0; block < 2; ++block)
                {
                    TestHelpers::fillWithSine (buffer, 96000.0, 220.0);
                    processor.processBlock (buffer, midi);

                    if (! TestHelpers::allSamplesFinite (buffer))
                        sawNonFiniteOutput.store (true, std::memory_order_relaxed);
                }
            }
        }

        stop.store (true, std::memory_order_relaxed);
    });

    // This test's own calling thread IS "the message thread" (whichever
    // thread first touches MessageManager::getInstance() - TestMain.cpp's
    // main() - becomes it, and JUCE asserts if runDispatchLoopUntil() is
    // called from any other thread). Pumping it here is what lets
    // NaveAudioProcessor::handleAsyncUpdate() actually fire, concurrently
    // with the host thread above - exactly as it would in a real host where
    // the JUCE message thread runs independently of whichever thread the
    // host calls prepareToPlay() from. If the race this test targets were
    // still present, the crash happens on JUCE's internal convolution
    // background-loader thread - a third thread besides these two - so no
    // try/catch here would be able to intercept it; the process would abort
    // exactly as it did in CI (see the class-level comment above).
    while (! stop.load (std::memory_order_relaxed))
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);

    automationThread.join();
    hostThread.join();

    REQUIRE_FALSE (sawNonFiniteOutput.load());
}
