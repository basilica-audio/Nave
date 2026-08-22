#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "ir/IrLibrary.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

// The bundled factory IR library (issue #33), measured rather than assumed.
//
// Nave ships nine cabinet impulse responses in resources/irs/, and all nine are
// GENERATED rather than recorded: tools/ir-synth/cabsynth.py computes each from
// a documented analytical cabinet model, so there is no third-party recording
// being redistributed and no provenance to trace or dispute. See
// resources/irs/LICENSES.md.
//
// They are MODELS, not captures. #33 made unambiguous labelling a binding
// condition on ever shipping synthetic IRs, so the "Modelled" prefix on every
// filename is a requirement rather than a style choice, and it is asserted
// below instead of being left to review.
//
// tools/ir-synth/verify_irs.py measures the same signal properties from the
// files on disk and gates CI on them. These cases exist because a measurement
// script proves the *files* are sound; only the plugin can prove the files are
// sound *through the plugin* - that they decode with the engine's own reader,
// that the browser's directory scan actually lists them, and that the
// convolution engine loads each one and audibly changes the signal.
//
// NAVE_IR_ASSET_DIR is set by CMakeLists.txt to the in-repo asset directory.
namespace
{
    constexpr double factorySampleRate = 48000.0;
    constexpr int factoryBlockSize = 512;

    juce::File factoryIrDirectory()
    {
        return juce::File (juce::String (NAVE_IR_ASSET_DIR));
    }

    juce::Array<juce::File> factoryIrFiles()
    {
        return basilica::ir::IrLibrary::scan (factoryIrDirectory());
    }

    struct DecodedIr
    {
        juce::String name;
        juce::AudioBuffer<float> buffer;
        double sampleRate = 0.0;
    };

    DecodedIr decodeIr (const juce::File& file)
    {
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        DecodedIr decoded;
        decoded.name = file.getFileName();

        const std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        REQUIRE (reader != nullptr);

        const auto numChannels = static_cast<int> (reader->numChannels);
        const auto numSamples = static_cast<int> (reader->lengthInSamples);

        decoded.buffer.setSize (numChannels, numSamples);
        decoded.buffer.clear();

        REQUIRE (reader->read (&decoded.buffer, 0, numSamples, 0, true, numChannels > 1));
        decoded.sampleRate = reader->sampleRate;

        return decoded;
    }

    // Peak of |H(f)| over a 65536-point transform.
    double peakMagnitudeResponse (const juce::AudioBuffer<float>& buffer)
    {
        constexpr int fftOrder = 16;
        constexpr int fftSize = 1 << fftOrder;

        REQUIRE (buffer.getNumSamples() <= fftSize);

        juce::dsp::FFT fft (fftOrder);
        std::vector<float> scratch (static_cast<size_t> (fftSize) * 2, 0.0f);

        const auto* source = buffer.getReadPointer (0);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            scratch[static_cast<size_t> (i)] = source[i];

        fft.performFrequencyOnlyForwardTransform (scratch.data());

        double peak = 0.0;
        for (int bin = 0; bin <= fftSize / 2; ++bin)
            peak = juce::jmax (peak, static_cast<double> (scratch[static_cast<size_t> (bin)]));

        return peak;
    }

    // Octave-band magnitude, in dB relative to the IR's own response peak -
    // the same quantity as the octave table in resources/irs/LICENSES.md, and
    // computed the same way: an RMS over the FFT bins inside the band.
    //
    // Deliberately NOT measured by running the IR through a second-order
    // band-pass, which was the first attempt here. A biquad band-pass has
    // 6 dB/octave skirts, so at 31.5 Hz most of what it integrates is leakage
    // from the 100-500 Hz region where a cabinet IR keeps nearly all of its
    // energy; two cabinets 9 dB apart down there measured 3 dB apart, because
    // the measurement was mostly reporting the octaves it was supposed to
    // reject. Selecting bins is exact and has no skirt at all.
    double octaveBandDb (const juce::AudioBuffer<float>& buffer, double centreHz)
    {
        constexpr int fftOrder = 16;
        constexpr int fftSize = 1 << fftOrder;

        REQUIRE (buffer.getNumSamples() <= fftSize);

        juce::dsp::FFT fft (fftOrder);
        std::vector<float> scratch (static_cast<size_t> (fftSize) * 2, 0.0f);

        const auto* source = buffer.getReadPointer (0);
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            scratch[static_cast<size_t> (i)] = source[i];

        fft.performFrequencyOnlyForwardTransform (scratch.data());

        const auto binHz = factorySampleRate / static_cast<double> (fftSize);
        const auto halfWidth = std::sqrt (2.0); // one octave

        const auto lowBin = juce::jmax (0, static_cast<int> (std::floor ((centreHz / halfWidth) / binHz)));
        const auto highBin = juce::jmin (fftSize / 2,
                                          static_cast<int> (std::ceil ((centreHz * halfWidth) / binHz)));

        REQUIRE (highBin >= lowBin);

        double bandEnergy = 0.0;
        double peak = 0.0;

        for (int bin = 0; bin <= fftSize / 2; ++bin)
        {
            const auto magnitude = static_cast<double> (scratch[static_cast<size_t> (bin)]);
            peak = juce::jmax (peak, magnitude);

            if (bin >= lowBin && bin <= highBin)
                bandEnergy += magnitude * magnitude;
        }

        const auto bandRms = std::sqrt (bandEnergy / (highBin - lowBin + 1));
        return juce::Decibels::gainToDecibels (bandRms / juce::jmax (1.0e-12, peak), -120.0);
    }
}

//==============================================================================
TEST_CASE ("Factory IR library: nine impulse responses ship, and the browser's scan finds them",
           "[ir][factory][content]")
{
    // The browser lists whatever IrLibrary::scan() returns for the library
    // folder, so scanning the shipped folder with the browser's own scanner is
    // the check that the library is actually browsable, not merely present.
    const auto directory = factoryIrDirectory();
    INFO ("asset directory: " << directory.getFullPathName());
    REQUIRE (directory.isDirectory());

    const auto files = factoryIrFiles();
    REQUIRE (files.size() == 9);

    juce::StringArray names;
    for (const auto& file : files)
        names.add (file.getFileName());

    for (const auto* expected : { "modelled_8x10_cone.wav",
                                   "modelled_8x10_edge.wav",
                                   "modelled_1x15_vintage.wav",
                                   "modelled_4x10_horn.wav",
                                   "modelled_4x12_ceramic_cone.wav",
                                   "modelled_4x12_ceramic_edge.wav",
                                   "modelled_4x12_ceramic_room.wav",
                                   "modelled_2x12_alnico_cone.wav",
                                   "modelled_1x12_combo_cone.wav" })
    {
        INFO ("expected asset: " << expected);
        CHECK (names.contains (expected));
    }
}

TEST_CASE ("Factory IR library: every bundled file is labelled as a model, not a capture",
           "[ir][factory][content][licence]")
{
    // Issue #33 made this binding: synthetic IRs may only ship if they cannot
    // be mistaken for real cabinet captures. The filename is what the browser
    // shows, so the filename is where the labelling has to hold, and it is
    // asserted rather than trusted.
    for (const auto& file : factoryIrFiles())
    {
        INFO ("asset: " << file.getFileName());
        CHECK (file.getFileName().startsWith ("modelled_"));
    }

    // The licence file and the machine-readable provenance record travel with
    // the audio; without them the set is undocumented regardless of how it
    // sounds.
    CHECK (factoryIrDirectory().getChildFile ("LICENSES.md").existsAsFile());
    CHECK (factoryIrDirectory().getChildFile ("CC0-1.0.txt").existsAsFile());
    CHECK (factoryIrDirectory().getChildFile ("manifest.json").existsAsFile());
}

TEST_CASE ("Factory IR library: every bundled asset decodes to the documented format",
           "[ir][factory][content]")
{
    for (const auto& file : factoryIrFiles())
    {
        const auto decoded = decodeIr (file);
        INFO ("asset: " << decoded.name);

        CHECK (decoded.sampleRate == Catch::Approx (48000.0));
        CHECK (decoded.buffer.getNumChannels() == 1);

        // 2048 taps for the guitar cabinets, 4096 for the bass cabinets (whose
        // lower-tuned alignments need the extra tail to null at DC), 8192 for
        // the room model. Anything else means the shipped audio is not what
        // resources/irs/LICENSES.md describes.
        const auto numSamples = decoded.buffer.getNumSamples();
        INFO ("length: " << numSamples);
        CHECK ((numSamples == 2048 || numSamples == 4096 || numSamples == 8192));
    }
}

TEST_CASE ("Factory IR library: no bundled asset clips, offsets, or goes non-finite",
           "[ir][factory][content]")
{
    for (const auto& file : factoryIrFiles())
    {
        const auto decoded = decodeIr (file);
        INFO ("asset: " << decoded.name);

        const auto* samples = decoded.buffer.getReadPointer (0);
        const auto numSamples = decoded.buffer.getNumSamples();

        double sum = 0.0;
        float peak = 0.0f;
        bool finite = true;

        for (int i = 0; i < numSamples; ++i)
        {
            finite = finite && std::isfinite (samples[i]);
            peak = juce::jmax (peak, std::abs (samples[i]));
            sum += static_cast<double> (samples[i]);
        }

        CHECK (finite);
        CHECK (peak < 1.0f);

        // A DC offset in an IR walks the convolved signal off centre and costs
        // headroom for nothing.
        CHECK (std::abs (sum / numSamples) < 1.0e-4);
    }
}

TEST_CASE ("Factory IR library: every bundled asset is normalised to unity peak response",
           "[ir][factory][content]")
{
    // max |H(f)| == 1.0 rather than a peak-sample or energy target: it is the
    // criterion that bounds the convolution, so no sine at any frequency can
    // leave the convolver louder than it entered. Nave's own Energy/Loudness
    // gain modes then do the perceptual level-matching on top (see
    // src/dsp/IrLoudness.h) - this is the safety property underneath them.
    for (const auto& file : factoryIrFiles())
    {
        const auto decoded = decodeIr (file);
        INFO ("asset: " << decoded.name);

        const auto peakDb = juce::Decibels::gainToDecibels (peakMagnitudeResponse (decoded.buffer));
        INFO ("peak magnitude response: " << peakDb << " dBFS");
        CHECK (std::abs (peakDb) < 0.1);
    }
}

TEST_CASE ("Factory IR library: every bundled asset decays and closes rather than being cut off",
           "[ir][factory][content]")
{
    // An IR still ringing on its last sample convolves as a step
    // discontinuity, which reads as broadband splatter. The generator applies a
    // raised-cosine fade for exactly that reason.
    for (const auto& file : factoryIrFiles())
    {
        const auto decoded = decodeIr (file);
        INFO ("asset: " << decoded.name);

        const auto* samples = decoded.buffer.getReadPointer (0);
        const auto numSamples = decoded.buffer.getNumSamples();

        for (int i = numSamples - 8; i < numSamples; ++i)
            CHECK (std::abs (samples[i]) < 1.0e-6f);

        double totalEnergy = 0.0;
        double tailEnergy = 0.0;
        const auto tailStart = static_cast<int> (numSamples * 0.9);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto energy = static_cast<double> (samples[i]) * samples[i];
            totalEnergy += energy;

            if (i >= tailStart)
                tailEnergy += energy;
        }

        REQUIRE (totalEnergy > 0.0);
        CHECK (tailEnergy / totalEnergy < 1.0e-4);
    }
}

TEST_CASE ("Factory IR library: the convolution engine loads every bundled asset",
           "[ir][factory][content][processor]")
{
    // The check the whole issue turns on: not "the file parses" but "the cab
    // engine loads this one and it is audible", for every file, through the
    // same message-thread API the browser calls.
    const auto renderWith = [] (const juce::File& irFile)
    {
        NaveAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, factorySampleRate, factoryBlockSize);
        processor.prepareToPlay (factorySampleRate, factoryBlockSize);

        if (irFile.existsAsFile())
            REQUIRE (processor.loadImpulseResponseFromFile (irFile));

        juce::AudioBuffer<float> buffer (2, factoryBlockSize);
        juce::AudioBuffer<float> tail (2, factoryBlockSize);

        // juce::dsp::Convolution (JUCE 8.0.14) prepares a newly loaded IR on
        // its own background thread and swaps it in on a later process() call,
        // so this waits on wall-clock time rather than spinning - the same
        // cadence tests/CoverageTests.cpp uses.
        for (int block = 0; block < 24; ++block)
        {
            juce::Thread::sleep (15);

            TestHelpers::fillWithSine (buffer, factorySampleRate, 900.0, 0.5f, block * factoryBlockSize);
            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
            tail.makeCopyOf (buffer);
        }

        return tail;
    };

    const auto differenceRms = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        juce::AudioBuffer<float> difference;
        difference.makeCopyOf (a);

        for (int channel = 0; channel < difference.getNumChannels(); ++channel)
            difference.addFrom (channel, 0, b, channel, 0, difference.getNumSamples(), -1.0f);

        return TestHelpers::rms (difference);
    };

    const auto dry = renderWith (juce::File());
    const auto dryRms = TestHelpers::rms (dry);
    REQUIRE (dryRms > 0.0);

    for (const auto& file : factoryIrFiles())
    {
        INFO ("asset: " << file.getFileName());

        const auto wet = renderWith (file);

        CHECK (TestHelpers::allSamplesFinite (wet));
        CHECK (TestHelpers::peakAbsolute (wet) < 4.0f);

        // A silently-failing load that left the delta IR in place would render
        // identically to the dry path; every cabinet has to actually be in
        // circuit.
        CHECK (differenceRms (wet, dry) > 0.05 * dryRms);
    }
}

TEST_CASE ("Factory IR library: the voicings are measurably different from one another",
           "[ir][factory][content]")
{
    // Nine slots that all measured the same would be a content failure every
    // other test here would happily pass. This pins the spread the set exists
    // for: the cone/edge pairs, the dark/bright extremes, and the room model's
    // decay.
    const auto load = [] (const juce::String& fileName)
    {
        return decodeIr (factoryIrDirectory().getChildFile (fileName));
    };

    const auto cone412 = load ("modelled_4x12_ceramic_cone.wav");
    const auto edge412 = load ("modelled_4x12_ceramic_edge.wav");
    const auto room412 = load ("modelled_4x12_ceramic_room.wav");
    const auto cone810 = load ("modelled_8x10_cone.wav");
    const auto edge810 = load ("modelled_8x10_edge.wav");
    const auto vintage115 = load ("modelled_1x15_vintage.wav");
    const auto horn410 = load ("modelled_4x10_horn.wav");

    // Cone vs edge, on both cabinets: same box, less top off-axis. This is the
    // difference the dual-slot IR Blend exists to exploit.
    INFO ("4x12 cone/edge at 4 kHz: " << octaveBandDb (cone412.buffer, 4000.0)
                                       << " / " << octaveBandDb (edge412.buffer, 4000.0));
    CHECK (octaveBandDb (cone412.buffer, 4000.0) > octaveBandDb (edge412.buffer, 4000.0) + 3.0);

    INFO ("8x10 cone/edge at 4 kHz: " << octaveBandDb (cone810.buffer, 4000.0)
                                       << " / " << octaveBandDb (edge810.buffer, 4000.0));
    CHECK (octaveBandDb (cone810.buffer, 4000.0) > octaveBandDb (edge810.buffer, 4000.0) + 3.0);

    // The dark end and the bright end of the bass set.
    INFO ("1x15 / 4x10+horn at 8 kHz: " << octaveBandDb (vintage115.buffer, 8000.0)
                                         << " / " << octaveBandDb (horn410.buffer, 8000.0));
    CHECK (octaveBandDb (horn410.buffer, 8000.0) > octaveBandDb (vintage115.buffer, 8000.0) + 6.0);

    // A bass cabinet has real energy where a guitar cabinet has almost none.
    // Measured at 31.5 Hz, not at 63 Hz: a sealed 4x12's own 82 Hz box
    // alignment keeps it within about a decibel of an 8x10 at 63 Hz and within
    // three at 40 Hz, so those are not the octaves where the two differ. An
    // octave lower, the 4x12's second-order roll-off has done its work and the
    // 8x10's vented low end has not.
    INFO ("8x10 / 4x12 at 31.5 Hz: " << octaveBandDb (cone810.buffer, 31.5)
                                      << " / " << octaveBandDb (cone412.buffer, 31.5));
    CHECK (octaveBandDb (cone810.buffer, 31.5) > octaveBandDb (cone412.buffer, 31.5) + 5.0);

    // The room model is the only one with a tail worth the name: past 50 ms it
    // still carries energy the close-miked models have long since faded out of.
    const auto energyAfter = [] (const juce::AudioBuffer<float>& buffer, int fromSample)
    {
        double energy = 0.0;
        const auto* samples = buffer.getReadPointer (0);

        for (int i = fromSample; i < buffer.getNumSamples(); ++i)
            energy += static_cast<double> (samples[i]) * samples[i];

        return energy;
    };

    const auto lateSample = static_cast<int> (0.05 * factorySampleRate);
    CHECK (room412.buffer.getNumSamples() > lateSample);
    CHECK (energyAfter (room412.buffer, lateSample) > 0.0);
    CHECK (energyAfter (room412.buffer, lateSample) > energyAfter (cone412.buffer, cone412.buffer.getNumSamples() - 1));
}
