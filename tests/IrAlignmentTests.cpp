#include "dsp/IrAlignment.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

namespace
{
    // A buffer that is silent up to `onsetSample`, then a short decaying
    // "transient" - close enough to a real cabinet IR's shape for onset
    // detection purposes.
    juce::AudioBuffer<float> makeBufferWithOnsetAt (int onsetSample, int totalSamples = 32)
    {
        juce::AudioBuffer<float> buffer (1, totalSamples);
        buffer.clear();

        for (int i = onsetSample; i < totalSamples; ++i)
            buffer.setSample (0, i, std::pow (0.7f, static_cast<float> (i - onsetSample)));

        return buffer;
    }
}

TEST_CASE ("detectOnsetSample finds a delayed transient's start", "[dsp][ir-alignment]")
{
    const auto buffer = makeBufferWithOnsetAt (10);
    CHECK (IrAlignment::detectOnsetSample (buffer) == 10);
}

TEST_CASE ("detectOnsetSample returns 0 for a transient starting at sample 0", "[dsp][ir-alignment]")
{
    const auto buffer = makeBufferWithOnsetAt (0);
    CHECK (IrAlignment::detectOnsetSample (buffer) == 0);
}

TEST_CASE ("detectOnsetSample returns 0 for a silent buffer", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> silent (1, 16);
    silent.clear();

    CHECK (IrAlignment::detectOnsetSample (silent) == 0);
}

TEST_CASE ("detectOnsetSample returns 0 for an empty buffer", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> empty (1, 0);
    CHECK (IrAlignment::detectOnsetSample (empty) == 0);
}

TEST_CASE ("shiftBySamples with a positive shift prepends silence and preserves content", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> buffer (1, 4);
    for (int i = 0; i < 4; ++i)
        buffer.setSample (0, i, static_cast<float> (i + 1));

    const auto shifted = IrAlignment::shiftBySamples (buffer, 3);

    REQUIRE (shifted.getNumSamples() == 7);
    CHECK (shifted.getSample (0, 0) == Catch::Approx (0.0f));
    CHECK (shifted.getSample (0, 1) == Catch::Approx (0.0f));
    CHECK (shifted.getSample (0, 2) == Catch::Approx (0.0f));
    CHECK (shifted.getSample (0, 3) == Catch::Approx (1.0f));
    CHECK (shifted.getSample (0, 4) == Catch::Approx (2.0f));
    CHECK (shifted.getSample (0, 5) == Catch::Approx (3.0f));
    CHECK (shifted.getSample (0, 6) == Catch::Approx (4.0f));
}

TEST_CASE ("shiftBySamples with a negative shift drops leading samples", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> buffer (1, 4);
    for (int i = 0; i < 4; ++i)
        buffer.setSample (0, i, static_cast<float> (i + 1));

    const auto shifted = IrAlignment::shiftBySamples (buffer, -2);

    REQUIRE (shifted.getNumSamples() == 2);
    CHECK (shifted.getSample (0, 0) == Catch::Approx (3.0f));
    CHECK (shifted.getSample (0, 1) == Catch::Approx (4.0f));
}

TEST_CASE ("shiftBySamples with zero shift returns an unmodified copy", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> buffer (1, 4);
    for (int i = 0; i < 4; ++i)
        buffer.setSample (0, i, static_cast<float> (i + 1));

    const auto shifted = IrAlignment::shiftBySamples (buffer, 0);

    REQUIRE (shifted.getNumSamples() == 4);
    for (int i = 0; i < 4; ++i)
        CHECK (shifted.getSample (0, i) == Catch::Approx (buffer.getSample (0, i)));
}

TEST_CASE ("shiftBySamples with an oversized negative shift clamps to at least one sample", "[dsp][ir-alignment]")
{
    juce::AudioBuffer<float> buffer (1, 4);
    for (int i = 0; i < 4; ++i)
        buffer.setSample (0, i, static_cast<float> (i + 1));

    const auto shifted = IrAlignment::shiftBySamples (buffer, -100);

    REQUIRE (shifted.getNumSamples() == 1);
    CHECK (shifted.getSample (0, 0) == Catch::Approx (4.0f)); // the last surviving sample
}

TEST_CASE ("alignOnsetToReference shifts a target IR so its onset matches the reference's, same sample rate", "[dsp][ir-alignment]")
{
    constexpr double sampleRate = 48000.0;

    // Reference (IR A) onset: sample 5. Target (IR B) onset: sample 20.
    const auto target = makeBufferWithOnsetAt (20, 64);

    const auto aligned = IrAlignment::alignOnsetToReference (target, sampleRate, 5, sampleRate);

    // The target must be advanced by (20 - 5) = 15 samples, so its onset now
    // lands at sample 5, matching the reference.
    CHECK (IrAlignment::detectOnsetSample (aligned) == 5);
}

TEST_CASE ("alignOnsetToReference handles differing sample rates by aligning in time, not raw samples", "[dsp][ir-alignment]")
{
    // Reference at 48 kHz, onset at sample 480 (10 ms). Target at 96 kHz,
    // onset at sample 480 (5 ms) - the same *sample index* but a different
    // *time*, so a naive sample-domain alignment would get this wrong.
    const auto target = makeBufferWithOnsetAt (480, 2000);

    const auto aligned = IrAlignment::alignOnsetToReference (target, 96000.0, 480, 48000.0);

    // Target needs to be delayed by 5 ms (10ms - 5ms) = 480 samples at its
    // own (96 kHz) rate, landing its onset at sample 960.
    CHECK (IrAlignment::detectOnsetSample (aligned) == 960);
}

TEST_CASE ("alignOnsetToReference is a no-op in onset terms when already aligned", "[dsp][ir-alignment]")
{
    constexpr double sampleRate = 44100.0;
    const auto target = makeBufferWithOnsetAt (12, 64);

    const auto aligned = IrAlignment::alignOnsetToReference (target, sampleRate, 12, sampleRate);

    CHECK (IrAlignment::detectOnsetSample (aligned) == 12);
}
