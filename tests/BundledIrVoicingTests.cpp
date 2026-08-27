#include "TestHelpers.h"
#include "dsp/IrAlignment.h"
#include "dsp/IrLoudness.h"
#include "ir/IrLibrary.h"

#include <juce_dsp/juce_dsp.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

// The bundled library's VOICING, pinned as a regression test (issue #47).
//
// #33 called curation "a listening decision" and #47 carried that forward as a
// listening pass over the nine. Most of what that pass would decide is
// measurable, and where it is, measurement is strictly stronger than ears: it
// is repeatable, it is independent of monitoring, and - unlike a listening
// session - it can be run again on every commit. tools/ir-synth/measure_irs.py
// performs the full characterisation and gates CI on it.
//
// These cases are the part of that characterisation which has to run HERE
// rather than in the script, because it is not a property of the files but of
// what the plugin does with them:
//
//   V1  POLARITY, as the engine detects it. IrAlignment::measure() is what
//       CabConvolutionEngine consults before blending slot B into slot A, and
//       MinPhase::estimateBulkDelaySamples() searches for a POSITIVE
//       correlation peak. An inverted bundled IR would partially cancel its
//       pair under Crossfade blend and would hand Morph a meaningless bulk
//       delay. Every one of the nine must read as upright, both on its own
//       direct arrival and against its family's reference cabinet.
//
//   V2  LEVEL MATCH, through the engine's own gain match. The level a user
//       hears when auditioning cabinets is NOT the level in the file: the
//       engine renormalises every IR at load, so a bundled IR's own gain never
//       reaches anybody. What is pinned is the residual spread after each of
//       the two gain modes, measured with IrLoudness - the same code the
//       plugin runs. It is a RATCHET, not a target; see the comment on
//       maxFamilySpreadDb.
//
//   V3  THE PAIRS. Three factory presets depend on guitar-412-cone/edge,
//       guitar-412-cone/room and bass-810-cone/edge (#44). Each pair has to
//       differ in the direction its names claim, and an aligned 50/50 blend
//       has to sum as correlated signals rather than comb.
//
//   V4  VOICING FOOTPRINT. Each cabinet's -10 dB band and its low-frequency
//       roll-off order, pinned so a re-bake of cabsynth.py cannot silently
//       change what a cabinet sounds like while still producing nine files
//       that pass every existing gate.
//
// ADR-0004's release policy is what makes this worth pinning: a bundled IR is
// never retuned in place, because presets resolve by a hash of its bytes. A
// revoicing ships as a new id and a new file. These assertions are the ratchet
// that turns that rule from a paragraph into a build failure.
namespace
{
    constexpr double irSampleRate = 48000.0;
    constexpr int fftOrder = 16;                 // 65536 points at 48 kHz: 0.73 Hz per bin
    constexpr int fftSize = 1 << fftOrder;
    constexpr int numBins = fftSize / 2 + 1;

    // Band edges are read relative to the mean level over this band rather than
    // to the response's single highest point. Every one of the nine is designed
    // to work inside it; below and above it the two families deliberately
    // diverge, which is what the edges are there to report. Referencing a peak
    // instead would measure the width of a cabinet's upper-mid resonance and
    // call it bandwidth.
    constexpr double referenceBandLowHz = 100.0;
    constexpr double referenceBandHighHz = 4000.0;

    // A third of an octave: the standard resolution for reporting loudspeaker
    // response. Reading an edge off an unsmoothed curve finds the first
    // reflection comb notch instead of the edge of the band.
    constexpr double smoothingFraction = 3.0;

    juce::File assetDirectory() { return juce::File (juce::String (NAVE_IR_ASSET_DIR)); }

    juce::AudioBuffer<float> decode (const juce::String& fileName)
    {
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        const auto file = assetDirectory().getChildFile (fileName);
        REQUIRE (file.existsAsFile());

        const std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
        REQUIRE (reader != nullptr);
        REQUIRE (reader->sampleRate == Catch::Approx (irSampleRate));

        juce::AudioBuffer<float> buffer (static_cast<int> (reader->numChannels),
                                          static_cast<int> (reader->lengthInSamples));
        reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
        return buffer;
    }

    std::vector<float> monoSamples (const juce::AudioBuffer<float>& buffer)
    {
        std::vector<float> out (static_cast<size_t> (buffer.getNumSamples()), 0.0f);
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                out[static_cast<size_t> (i)] += data[i];
        }
        return out;
    }

    // Power spectrum on a 65536-point grid, then power-averaged over a
    // third-octave band centred on each bin.
    std::vector<double> smoothedPowerSpectrum (const std::vector<float>& impulse)
    {
        juce::dsp::FFT fft (fftOrder);
        std::vector<float> scratch (static_cast<size_t> (fftSize) * 2, 0.0f);
        std::copy (impulse.begin(),
                   impulse.begin() + std::min (impulse.size(), static_cast<size_t> (fftSize)),
                   scratch.begin());
        fft.performFrequencyOnlyForwardTransform (scratch.data());

        std::vector<double> power (static_cast<size_t> (numBins), 0.0);
        for (int bin = 0; bin < numBins; ++bin)
        {
            const auto magnitude = static_cast<double> (scratch[static_cast<size_t> (bin)]);
            power[static_cast<size_t> (bin)] = magnitude * magnitude;
        }

        std::vector<double> cumulative (static_cast<size_t> (numBins) + 1, 0.0);
        for (int bin = 0; bin < numBins; ++bin)
            cumulative[static_cast<size_t> (bin) + 1] = cumulative[static_cast<size_t> (bin)]
                                                       + power[static_cast<size_t> (bin)];

        const auto halfWidth = std::pow (2.0, 1.0 / (2.0 * smoothingFraction));
        std::vector<double> smoothed (static_cast<size_t> (numBins), 0.0);
        smoothed[0] = power[0];
        for (int bin = 1; bin < numBins; ++bin)
        {
            const auto low = std::max (1, static_cast<int> (std::floor (bin / halfWidth)));
            const auto high = std::min (numBins - 1, static_cast<int> (std::ceil (bin * halfWidth)));
            smoothed[static_cast<size_t> (bin)] = (cumulative[static_cast<size_t> (high) + 1]
                                                   - cumulative[static_cast<size_t> (low)])
                                                  / static_cast<double> (high - low + 1);
        }
        return smoothed;
    }

    double binFrequency (int bin) { return bin * irSampleRate / fftSize; }

    double toDb (double power) { return 10.0 * std::log10 (std::max (power, 1.0e-30)); }

    double bandMeanDb (const std::vector<double>& smoothed, double lowHz, double highHz)
    {
        double total = 0.0;
        int count = 0;
        for (int bin = 1; bin < numBins; ++bin)
        {
            const auto frequency = binFrequency (bin);
            if (frequency >= lowHz && frequency <= highHz)
            {
                total += smoothed[static_cast<size_t> (bin)];
                ++count;
            }
        }
        return toDb (total / std::max (count, 1));
    }

    double levelAtDb (const std::vector<double>& smoothed, double frequencyHz)
    {
        const auto bin = std::clamp (static_cast<int> (std::lround (frequencyHz * fftSize / irSampleRate)),
                                      1, numBins - 1);
        return toDb (smoothed[static_cast<size_t> (bin)]);
    }

    struct BandEdges { double lowHz = 0.0, highHz = 0.0; };

    BandEdges bandEdges (const std::vector<double>& smoothed, double referenceDb, double dropDb)
    {
        BandEdges edges;
        for (int bin = 1; bin < numBins; ++bin)
        {
            const auto frequency = binFrequency (bin);
            if (frequency < 15.0 || frequency > 22000.0)
                continue;
            if (toDb (smoothed[static_cast<size_t> (bin)]) >= referenceDb - dropDb)
            {
                if (edges.lowHz <= 0.0)
                    edges.lowHz = frequency;
                edges.highHz = frequency;
            }
        }
        return edges;
    }

    // Schroeder backward integration, in dB relative to the IR's total energy.
    std::vector<double> energyDecayCurveDb (const std::vector<float>& impulse)
    {
        double total = 0.0;
        for (auto sample : impulse)
            total += static_cast<double> (sample) * sample;

        std::vector<double> curve (impulse.size(), 0.0);
        double running = 0.0;
        for (auto index = static_cast<int> (impulse.size()) - 1; index >= 0; --index)
        {
            running += static_cast<double> (impulse[static_cast<size_t> (index)])
                       * impulse[static_cast<size_t> (index)];
            curve[static_cast<size_t> (index)] = toDb (running / std::max (total, 1.0e-30));
        }
        return curve;
    }

    double t20Ms (const std::vector<float>& impulse)
    {
        const auto curve = energyDecayCurveDb (impulse);
        int start = -1, end = -1;
        for (size_t index = 0; index < curve.size(); ++index)
        {
            if (start < 0 && curve[index] <= -5.0)
                start = static_cast<int> (index);
            if (end < 0 && curve[index] <= -25.0)
                end = static_cast<int> (index);
        }
        if (start < 0 || end <= start)
            return 0.0;
        return (end - start) / irSampleRate * 1000.0 * 3.0;
    }

    // The two gain matches the engine applies at load, expressed as gains on
    // the shipped bytes. Energy is juce::dsp::Convolution::Normalise::yes -
    // JUCE 8.0.14's calculateNormalisationFactor() is
    // 0.125f / sqrt(sumSquaredMagnitude), which is exactly
    // IrLoudness::computeEnergyGain(). Loudness is
    // IrLoudness::computeLoudnessGain().
    double energyModeGainDb (const juce::AudioBuffer<float>& buffer)
    {
        return 20.0 * std::log10 (static_cast<double> (IrLoudness::computeEnergyGain (buffer)));
    }

    double loudnessModeGainDb (const juce::AudioBuffer<float>& buffer)
    {
        return 20.0 * std::log10 (static_cast<double> (IrLoudness::computeLoudnessGain (buffer, irSampleRate)));
    }

    // K-weighted loudness of a pink source through this IR, evaluated in the
    // frequency domain so there is no test signal, no seed and no run-to-run
    // variance:
    //     L = 10 log10( SUM S|H|^2|K|^2 / SUM S|K|^2 )
    // |K| is measured from IrLoudness's own biquads, so this test and the
    // plugin agree on what "loudness" means by construction. The source is
    // band-limited pink because a guitar or bass preamp feeding a cabinet is
    // roughly pink over the band the cabinet reproduces, and energy outside it
    // survives none of these cabinets.
    double kWeightedPowerAt (int bin)
    {
        static const std::vector<double> response = []
        {
            const auto stage1 = IrLoudness::makeStage1ShelfCoefficients (irSampleRate);
            const auto stage2 = IrLoudness::makeStage2HighPassCoefficients (irSampleRate);

            std::vector<double> out (static_cast<size_t> (numBins), 0.0);
            for (int bin = 0; bin < numBins; ++bin)
            {
                const auto omega = 2.0 * juce::MathConstants<double>::pi * bin / fftSize;
                const std::complex<double> z1 { std::cos (-omega), std::sin (-omega) };
                const auto z2 = z1 * z1;

                const auto evaluate = [&] (const IrLoudness::BiquadCoefficients& c)
                {
                    return std::abs ((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2));
                };

                const auto magnitude = evaluate (stage1) * evaluate (stage2);
                out[static_cast<size_t> (bin)] = magnitude * magnitude;
            }
            return out;
        }();
        return response[static_cast<size_t> (bin)];
    }

    double pinkLoudnessDb (const std::vector<float>& impulse, double lowHz, double highHz)
    {
        juce::dsp::FFT fft (fftOrder);
        std::vector<float> scratch (static_cast<size_t> (fftSize) * 2, 0.0f);
        std::copy (impulse.begin(),
                   impulse.begin() + std::min (impulse.size(), static_cast<size_t> (fftSize)),
                   scratch.begin());
        fft.performFrequencyOnlyForwardTransform (scratch.data());

        double signal = 0.0, reference = 0.0;
        for (int bin = 1; bin < numBins; ++bin)
        {
            const auto frequency = binFrequency (bin);
            if (frequency < lowHz || frequency > highHz)
                continue;
            const auto weight = kWeightedPowerAt (bin) / frequency;   // pink: power ~ 1/f
            const auto magnitude = static_cast<double> (scratch[static_cast<size_t> (bin)]);
            signal += magnitude * magnitude * weight;
            reference += weight;
        }
        return toDb (signal / std::max (reference, 1.0e-30));
    }

    struct Cabinet
    {
        const char* id;
        const char* file;
        const char* family;
        const char* enclosure;      // sealed | ported | open-back
        double lowEdgeHz;           // measured -10 dB low edge
        double highEdgeHz;          // measured -10 dB high edge
        double t20;                 // measured T20, ms
    };

    // The measured character of each shipped cabinet, from
    // tools/ir-synth/measure_irs.py against the bytes in resources/irs/. These
    // are not targets the generator was aimed at - they are what the files
    // actually do, recorded so that a change to them has to be deliberate.
    const std::vector<Cabinet>& cabinets()
    {
        static const std::vector<Cabinet> all {
            { "guitar-412-cone",   "modelled_4x12_ceramic_cone.wav", "guitar", "sealed",      44.0,  6735.0,  23.1 },
            { "guitar-412-edge",   "modelled_4x12_ceramic_edge.wav", "guitar", "sealed",      40.0,  4589.0,  25.5 },
            { "guitar-412-room",   "modelled_4x12_ceramic_room.wav", "guitar", "sealed",      57.0,  5834.0, 319.2 },
            { "guitar-212-alnico", "modelled_2x12_alnico_cone.wav",  "guitar", "open-back",   90.0,  7467.0,   7.7 },
            { "guitar-112-combo",  "modelled_1x12_combo_cone.wav",   "guitar", "open-back",   83.0,  7180.0,   9.9 },
            { "bass-810-cone",     "modelled_8x10_cone.wav",         "bass",   "sealed",      26.0,  5631.0,  33.2 },
            { "bass-810-edge",     "modelled_8x10_edge.wav",         "bass",   "sealed",      25.0,  3837.0,  34.8 },
            { "bass-115-vintage",  "modelled_1x15_vintage.wav",      "bass",   "ported",      28.0,  3763.0,  63.0 },
            { "bass-410-horn",     "modelled_4x10_horn.wav",         "bass",   "ported",      31.0, 12595.0,  49.4 },
        };
        return all;
    }

    // How far a re-bake may move a band edge before it counts as a revoicing.
    // A sixth of an octave is twice the third-octave smoothing kernel's own
    // resolution, so it cannot be tripped by the analysis, and it is a quarter
    // of the 0.55-octave difference the cone/edge pairs carry - so a change
    // large enough to alter which cabinet is the darker one fails first.
    constexpr double bandEdgeToleranceOctaves = 1.0 / 6.0;

    // The pink source each family's loudness is read through.
    constexpr double guitarBandLowHz = 70.0, guitarBandHighHz = 6000.0;
    constexpr double bassBandLowHz = 30.0, bassBandHighHz = 5000.0;
}

// V1 - polarity.
TEST_CASE ("Every bundled cabinet is upright, on its own and against its family",
           "[ir][voicing][polarity]")
{
    std::map<std::string, juce::AudioBuffer<float>> buffers;
    for (const auto& cabinet : cabinets())
        buffers.emplace (cabinet.id, decode (cabinet.file));

    for (const auto& cabinet : cabinets())
    {
        const auto samples = monoSamples (buffers.at (cabinet.id));

        // The direct arrival. A cabinet IR has one dominant transient inside
        // the first millisecond; its sign is the file's polarity.
        const auto window = static_cast<size_t> (0.001 * irSampleRate);
        const auto onset = std::max_element (samples.begin(),
                                             samples.begin() + static_cast<long> (window),
                                             [] (float a, float b) { return std::abs (a) < std::abs (b); });

        INFO ("cabinet " << cabinet.id << " direct arrival " << *onset);
        CHECK (*onset > 0.0f);
    }

    // And as the engine sees it. IrAlignment::measure() is what
    // CabConvolutionEngine::setImpulseResponseB() consults before blending;
    // a bundled IR that read as inverted against its own family's reference
    // would blend hollow.
    for (const auto& cabinet : cabinets())
    {
        const std::string reference = std::string (cabinet.family) == "guitar" ? "guitar-412-cone"
                                                                              : "bass-810-cone";
        if (cabinet.id == reference)
            continue;

        const auto measurement = IrAlignment::measure (buffers.at (reference), irSampleRate,
                                                       buffers.at (cabinet.id), irSampleRate);
        INFO ("cabinet " << cabinet.id << " against " << reference
                         << ", correlation " << measurement.normalisedPeak);
        CHECK_FALSE (measurement.polarityInverted);
    }
}

// V2 - level match, through the engine's own gain match.
TEST_CASE ("Auditioning one bundled cabinet after another does not jump in level",
           "[ir][voicing][level]")
{
    // A RATCHET, not a target. The target is 1 dB - the level JND for
    // broadband programme material (Zwicker & Fastl, Psychoacoustics ch. 8),
    // below which no cabinet can win an A/B on level alone. The set does not
    // meet it, and cannot be made to by any change to these files: the engine
    // discards each IR's own level at load, so the residual spread belongs to
    // the gain match, which references a WHITE excitation and is therefore
    // exact for white and drifts for any tilted source. Rescaling the WAVs
    // would change nothing audible and would break every preset that
    // references them by byte hash (ADR-0004). What is gated here is that the
    // spread does not get worse: the measured worst case is 4.20 dB, and the
    // bound is the next half-decibel above it.
    constexpr double maxFamilySpreadDb = 4.5;

    struct Reading { double energyMode = 0.0, loudnessMode = 0.0; };
    std::map<std::string, std::vector<Reading>> byFamily;

    for (const auto& cabinet : cabinets())
    {
        const auto buffer = decode (cabinet.file);
        const auto samples = monoSamples (buffer);
        const auto guitar = std::string (cabinet.family) == "guitar";
        const auto shipped = pinkLoudnessDb (samples,
                                             guitar ? guitarBandLowHz : bassBandLowHz,
                                             guitar ? guitarBandHighHz : bassBandHighHz);

        byFamily[cabinet.family].push_back ({ shipped + energyModeGainDb (buffer),
                                              shipped + loudnessModeGainDb (buffer) });
    }

    for (const auto& [family, readings] : byFamily)
    {
        const auto spread = [&readings] (auto member)
        {
            auto low = std::numeric_limits<double>::max();
            auto high = std::numeric_limits<double>::lowest();
            for (const auto& reading : readings)
            {
                low = std::min (low, reading.*member);
                high = std::max (high, reading.*member);
            }
            return high - low;
        };

        INFO ("family " << family
                        << " Energy-mode spread " << spread (&Reading::energyMode)
                        << " dB, Loudness-mode spread " << spread (&Reading::loudnessMode) << " dB");
        CHECK (spread (&Reading::energyMode) <= maxFamilySpreadDb);
        CHECK (spread (&Reading::loudnessMode) <= maxFamilySpreadDb);
    }
}

// V3 - the pairs three factory presets depend on.
TEST_CASE ("The cone/edge and cone/room pairs differ in the direction their names claim",
           "[ir][voicing][pairs]")
{
    struct Voicing
    {
        std::vector<float> samples;
        std::vector<double> smoothed;
        double referenceDb = 0.0;
        BandEdges tenDb;
        double presenceTiltDb = 0.0;
        double t20 = 0.0;
    };

    const auto analyse = [] (const char* file)
    {
        Voicing voicing;
        voicing.samples = monoSamples (decode (file));
        voicing.smoothed = smoothedPowerSpectrum (voicing.samples);
        voicing.referenceDb = bandMeanDb (voicing.smoothed, referenceBandLowHz, referenceBandHighHz);
        voicing.tenDb = bandEdges (voicing.smoothed, voicing.referenceDb, 10.0);
        voicing.presenceTiltDb = bandMeanDb (voicing.smoothed, 2000.0, 5000.0)
                                 - bandMeanDb (voicing.smoothed, 200.0, 800.0);
        voicing.t20 = t20Ms (voicing.samples);
        return voicing;
    };

    SECTION ("an edge capture is darker than its cone capture")
    {
        // A microphone moved from the dust cap to the cone edge and angled
        // off-axis loses high end - cone directivity, and the entire claim both
        // "edge" files make. Two independent readings of the same claim are
        // checked because either alone is brittle: the -10 dB edge is one
        // number off a smoothed curve (both pairs measure 0.55 octaves, so a
        // third of an octave leaves 60% of margin), while the 2-5 kHz tilt
        // integrates over the whole presence region (both pairs measure
        // 6.5-7.2 dB, so 3 dB leaves better than a factor of two).
        constexpr double minDarkerOctaves = 0.35;
        constexpr double minPresenceDropDb = 3.0;

        for (const auto& [coneFile, edgeFile, label] : std::vector<std::tuple<const char*, const char*, const char*>> {
                 { "modelled_4x12_ceramic_cone.wav", "modelled_4x12_ceramic_edge.wav", "guitar 4x12" },
                 { "modelled_8x10_cone.wav", "modelled_8x10_edge.wav", "bass 8x10" } })
        {
            const auto cone = analyse (coneFile);
            const auto edge = analyse (edgeFile);

            const auto octaves = std::log2 (cone.tenDb.highHz / edge.tenDb.highHz);
            const auto presenceDrop = cone.presenceTiltDb - edge.presenceTiltDb;

            INFO (label << ": -10 dB high edge " << cone.tenDb.highHz << " -> " << edge.tenDb.highHz
                        << " Hz (" << octaves << " octaves), 2-5 kHz tilt "
                        << cone.presenceTiltDb << " -> " << edge.presenceTiltDb << " dB");
            CHECK (octaves >= minDarkerOctaves);
            CHECK (presenceDrop >= minPresenceDropDb);
        }
    }

    SECTION ("the room capture decays like a room and the close capture does not")
    {
        // A factor of four is far outside what a reflection pattern alone
        // produces and far inside the factor of thirteen the pair delivers.
        constexpr double minDecayRatio = 4.0;

        const auto cone = analyse ("modelled_4x12_ceramic_cone.wav");
        const auto room = analyse ("modelled_4x12_ceramic_room.wav");

        INFO ("T20 " << cone.t20 << " -> " << room.t20 << " ms");
        CHECK (room.t20 / cone.t20 >= minDecayRatio);
    }

    SECTION ("an aligned 50/50 blend of a pair sums coherently rather than combing")
    {
        // Crossfade blend sums the two convolver branches after IrAlignment has
        // put slot B's onset on slot A's, so this is what the engine does:
        // align, level-match, sum at 0.5 each. Perfectly coherent and perfectly
        // incoherent summing differ by 3.0 dB, so the whole scale of the defect
        // is 3 dB; a sixth of that is below the ~1 dB level JND and an order of
        // magnitude away from an audible comb.
        constexpr double maxCoherenceLossDb = 0.5;

        for (const auto& [aFile, bFile, guitar, label] :
             std::vector<std::tuple<const char*, const char*, bool, const char*>> {
                 { "modelled_4x12_ceramic_cone.wav", "modelled_4x12_ceramic_edge.wav", true, "guitar cone+edge" },
                 { "modelled_4x12_ceramic_cone.wav", "modelled_4x12_ceramic_room.wav", true, "guitar cone+room" },
                 { "modelled_8x10_cone.wav", "modelled_8x10_edge.wav", false, "bass cone+edge" } })
        {
            auto a = decode (aFile);
            auto b = decode (bFile);

            const auto measurement = IrAlignment::measure (a, irSampleRate, b, irSampleRate);
            auto aligned = IrAlignment::shiftByFractionalSamples (b, -measurement.lagSamples);

            a.applyGain (IrLoudness::computeEnergyGain (a));
            aligned.applyGain (IrLoudness::computeEnergyGain (aligned));

            const auto left = monoSamples (a);
            const auto right = monoSamples (aligned);
            std::vector<float> summed (std::max (left.size(), right.size()), 0.0f);
            for (size_t i = 0; i < summed.size(); ++i)
            {
                const auto u = i < left.size() ? left[i] : 0.0f;
                const auto v = i < right.size() ? right[i] : 0.0f;
                summed[i] = 0.5f * (u + v);
            }

            const auto lowHz = guitar ? guitarBandLowHz : bassBandLowHz;
            const auto highHz = guitar ? guitarBandHighHz : bassBandHighHz;
            const auto loudnessA = pinkLoudnessDb (left, lowHz, highHz);
            const auto loudnessB = pinkLoudnessDb (right, lowHz, highHz);
            const auto loudnessSum = pinkLoudnessDb (summed, lowHz, highHz);

            const auto amplitude = 0.5 * (std::pow (10.0, loudnessA / 20.0)
                                          + std::pow (10.0, loudnessB / 20.0));
            const auto coherentDb = 20.0 * std::log10 (amplitude);

            INFO (label << ": lag " << measurement.lagSamples << ", sum " << loudnessSum
                        << " dB against a coherent " << coherentDb << " dB");
            CHECK (loudnessSum - coherentDb >= -maxCoherenceLossDb);
        }
    }
}

// V4 - the voicing footprint of each cabinet.
TEST_CASE ("Each bundled cabinet still occupies the band and rolls off at the order it shipped with",
           "[ir][voicing][bandwidth]")
{
    for (const auto& cabinet : cabinets())
    {
        const auto samples = monoSamples (decode (cabinet.file));
        const auto smoothed = smoothedPowerSpectrum (samples);
        const auto referenceDb = bandMeanDb (smoothed, referenceBandLowHz, referenceBandHighHz);
        const auto tenDb = bandEdges (smoothed, referenceDb, 10.0);
        const auto threeDb = bandEdges (smoothed, referenceDb, 3.0);

        INFO ("cabinet " << cabinet.id << ": -10 dB band " << tenDb.lowHz << " - " << tenDb.highHz
                         << " Hz, expected " << cabinet.lowEdgeHz << " - " << cabinet.highEdgeHz);
        CHECK (std::abs (std::log2 (tenDb.lowHz / cabinet.lowEdgeHz)) <= bandEdgeToleranceOctaves);
        CHECK (std::abs (std::log2 (tenDb.highHz / cabinet.highEdgeHz)) <= bandEdgeToleranceOctaves);

        // The roll-off order at the cabinet's own -3 dB corner is set by the
        // enclosure rather than by voicing EQ, which is what makes it the
        // discriminator for structural variety:
        //   open-back  front and rear waves cancel: first order, 6 dB/octave
        //   sealed     one second-order box alignment: 12 dB/octave
        //   ported     a vented alignment is fourth order below tuning: 24
        // The bounds sit in the gaps the measurements leave - the widest
        // open-back is 8.5, the narrowest sealed 13.4, the widest sealed 14.6,
        // the narrowest ported 25.4 - so each carries at least 4 dB/octave.
        const auto slope = levelAtDb (smoothed, threeDb.lowHz) - levelAtDb (smoothed, threeDb.lowHz / 2.0);
        INFO ("cabinet " << cabinet.id << " is " << cabinet.enclosure << " and rolls off at "
                         << slope << " dB/octave below " << threeDb.lowHz << " Hz");

        if (std::string (cabinet.enclosure) == "open-back")
            CHECK (slope <= 10.0);
        else if (std::string (cabinet.enclosure) == "sealed")
            CHECK ((slope >= 11.0 && slope <= 18.0));
        else
            CHECK (slope >= 21.0);

        // And the decay it shipped with, to a fifth - wide enough that the
        // Schroeder estimator's own sensitivity to the noise floor cannot trip
        // it, narrow enough that the room cabinet could not become a close one.
        const auto measured = t20Ms (samples);
        INFO ("cabinet " << cabinet.id << " T20 " << measured << " ms, expected " << cabinet.t20);
        CHECK (std::abs (measured - cabinet.t20) <= 0.2 * cabinet.t20);
    }
}
