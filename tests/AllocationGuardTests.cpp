#include "dsp/CabConvolutionEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cstdlib>
#include <new>

// Audio-thread allocation guard.
//
// Allocating on the audio thread is the classic real-time bug: malloc can take
// a lock, and a lock can make the audio callback miss its deadline, which the
// user hears as a dropout. It is also invisible in ordinary testing - the code
// works perfectly until the machine is under load.
//
// The only reliable way to catch it is to replace the global allocator and
// count. The counter is armed for exactly the duration of a process() call, so
// allocation anywhere else (prepare(), the test harness, Catch2's own
// bookkeeping) is ignored.
//
// This test is a MERGE GATE, and it fails by design against v0.2's
// '*filter.state = *IIR::Coefficients<float>::make...' idiom, which
// heap-allocates a ref-counted coefficients object on every engaged block.
// v0.3.0's ArrayCoefficients replacement (see the note at the top of
// CabConvolutionEngine.cpp) is the prerequisite for it passing - so this
// asserts an improvement over v0.2, not the status quo.
namespace
{
    std::atomic<int> audioThreadAllocations { 0 };
    std::atomic<bool> guardArmed { false };

    void recordAllocation() noexcept
    {
        if (guardArmed.load (std::memory_order_relaxed))
            audioThreadAllocations.fetch_add (1, std::memory_order_relaxed);
    }

    // Arms the counter for the lifetime of the scope. Deliberately RAII: an
    // early return or a thrown assertion inside a guarded block must still
    // disarm, or every later test in the binary would start counting.
    struct ScopedAllocationGuard
    {
        ScopedAllocationGuard()
        {
            audioThreadAllocations.store (0, std::memory_order_relaxed);
            guardArmed.store (true, std::memory_order_relaxed);
        }

        ~ScopedAllocationGuard()
        {
            guardArmed.store (false, std::memory_order_relaxed);
        }

        int count() const noexcept { return audioThreadAllocations.load (std::memory_order_relaxed); }
    };

    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 512;
}

// Replacing the global allocation functions is explicitly permitted by the
// standard, and applies to the whole Tests binary - which is what makes this
// guard catch allocations inside JUCE as well as inside Nave's own code.
void* operator new (std::size_t size)
{
    recordAllocation();

    if (auto* pointer = std::malloc (size == 0 ? 1 : size))
        return pointer;

    throw std::bad_alloc();
}

void* operator new[] (std::size_t size)
{
    recordAllocation();

    if (auto* pointer = std::malloc (size == 0 ? 1 : size))
        return pointer;

    throw std::bad_alloc();
}

void* operator new (std::size_t size, const std::nothrow_t&) noexcept
{
    recordAllocation();
    return std::malloc (size == 0 ? 1 : size);
}

void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept
{
    recordAllocation();
    return std::malloc (size == 0 ? 1 : size);
}

void operator delete (void* pointer) noexcept { std::free (pointer); }
void operator delete[] (void* pointer) noexcept { std::free (pointer); }
void operator delete (void* pointer, std::size_t) noexcept { std::free (pointer); }
void operator delete[] (void* pointer, std::size_t) noexcept { std::free (pointer); }
void operator delete (void* pointer, const std::nothrow_t&) noexcept { std::free (pointer); }
void operator delete[] (void* pointer, const std::nothrow_t&) noexcept { std::free (pointer); }

//==============================================================================
TEST_CASE ("The allocation guard itself detects an allocation", "[dsp][allocation-guard]")
{
    // Without this, a guard that silently stopped counting would make every
    // assertion below pass vacuously.
    ScopedAllocationGuard guard;

    auto* leaked = new int (42);
    const auto counted = guard.count();
    delete leaked;

    CHECK (counted > 0);
}

//==============================================================================
// Test 22 (merge gate): process() allocates nothing with every feature engaged.
TEST_CASE ("process() performs no audio-thread allocations with all features engaged",
           "[dsp][allocation-guard]")
{
    CabConvolutionEngine engine;

    // Everything on at once, and specifically the combination the brief calls
    // out: 24 dB/oct slopes WITH LoCut, HiCut and Distance all engaged, so
    // every coefficient-update path runs, plus blend automating and the morph
    // path active with a fade in flight.
    engine.setLoCutHz (120.0f);
    engine.setHiCutHz (6000.0f);
    engine.setLoCutSlope (CabConvolutionEngine::Slope::TwentyFourDbPerOctave);
    engine.setHiCutSlope (CabConvolutionEngine::Slope::TwentyFourDbPerOctave);
    engine.setDistancePercent (60.0f);
    engine.setDistanceAirEnabled (true);
    engine.setMixProportion (0.8f);
    engine.setLevelDb (-1.5f);
    engine.setBlendProportion (0.5f);
    engine.setIrBTrimDb (-3.0f);
    engine.setIrBPolarityInverted (true);
    engine.setIrBDelayMs (1.25f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = testSampleRate;
    spec.maximumBlockSize = testBlockSize;
    spec.numChannels = 2;

    // Real IRs in both slots, so both convolvers and the morph engine have
    // actual content to run.
    juce::AudioBuffer<float> irA (1, 512);
    juce::AudioBuffer<float> irB (1, 512);

    irA.clear();
    irB.clear();

    for (int i = 0; i < 512; ++i)
    {
        const auto decay = std::exp (-5.0f * static_cast<float> (i) / 512.0f);
        irA.setSample (0, i, decay * std::sin (0.05f * static_cast<float> (i)));
        irB.setSample (0, i, decay * std::cos (0.031f * static_cast<float> (i)));
    }

    irA.setSample (0, 0, 1.0f);
    irB.setSample (0, 0, 0.9f);

    engine.setImpulseResponse (irA, testSampleRate);
    engine.setImpulseResponseB (irB, testSampleRate);

    engine.setBlendMode (CabConvolutionEngine::BlendMode::Morph);

    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, testBlockSize);

    // Warm-up passes OUTSIDE the guard: the first few blocks legitimately
    // allocate (JUCE's convolution engine finishes its background load, the
    // morph worker publishes its first IR). What must be allocation-free is
    // the steady state, which is what the guarded loop below measures.
    for (int blockIndex = 0; blockIndex < 40; ++blockIndex)
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, 440.0, 0.5f,
                                    static_cast<juce::int64> (blockIndex) * testBlockSize);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);
    }

    int allocations = 0;

    {
        ScopedAllocationGuard guard;

        for (int blockIndex = 0; blockIndex < 64; ++blockIndex)
        {
            // Automate Blend continuously, so the per-sample gain path and the
            // engage/disengage transitions are exercised inside the guard.
            engine.setBlendProportion (0.5f + 0.4f * std::sin (0.2f * static_cast<float> (blockIndex)));

            // And sweep the cut frequencies, so the 32-sample coefficient
            // update cadence runs on every sub-block rather than sitting on a
            // static value.
            engine.setLoCutHz (120.0f + 60.0f * std::sin (0.11f * static_cast<float> (blockIndex)));
            engine.setHiCutHz (6000.0f + 900.0f * std::cos (0.07f * static_cast<float> (blockIndex)));

            TestHelpers::fillWithSine (buffer, testSampleRate, 440.0, 0.5f,
                                        static_cast<juce::int64> (blockIndex) * testBlockSize);

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);
        }

        allocations = guard.count();
    }

    CAPTURE (allocations);

    CHECK (allocations == 0);
    CHECK (TestHelpers::allSamplesFinite (buffer));
}

//==============================================================================
// The same guarantee for the plain Crossfade path, so a regression in the
// non-morph configuration cannot hide behind the morph test above.
TEST_CASE ("process() performs no audio-thread allocations in Crossfade mode",
           "[dsp][allocation-guard]")
{
    CabConvolutionEngine engine;

    engine.setLoCutHz (200.0f);
    engine.setHiCutHz (5000.0f);
    engine.setDistancePercent (35.0f);
    engine.setMixProportion (0.6f);
    engine.setBlendProportion (0.4f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = testSampleRate;
    spec.maximumBlockSize = testBlockSize;
    spec.numChannels = 2;

    juce::AudioBuffer<float> ir (1, 256);
    ir.clear();
    ir.setSample (0, 0, 1.0f);
    ir.setSample (0, 64, -0.4f);

    engine.setImpulseResponse (ir, testSampleRate);
    engine.setImpulseResponseB (ir, testSampleRate);

    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, testBlockSize);

    for (int blockIndex = 0; blockIndex < 24; ++blockIndex)
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, 440.0, 0.5f,
                                    static_cast<juce::int64> (blockIndex) * testBlockSize);

        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);
    }

    int allocations = 0;

    {
        ScopedAllocationGuard guard;

        for (int blockIndex = 0; blockIndex < 32; ++blockIndex)
        {
            TestHelpers::fillWithSine (buffer, testSampleRate, 440.0, 0.5f,
                                        static_cast<juce::int64> (blockIndex) * testBlockSize);

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);
        }

        allocations = guard.count();
    }

    CAPTURE (allocations);
    CHECK (allocations == 0);
}
