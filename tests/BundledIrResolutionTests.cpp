#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "ir/BundledIrSource.h"
#include "ir/FactoryIrLibrary.h"
#include "ir/IrLibrary.h"
#include "params/ParameterIds.h"
#include "presets/IrReference.h"
#include "presets/PresetManager.h"

#include <BinaryData.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

// Resolving a preset's IR reference from the EMBEDDED bundle (issue #45).
//
// The decisions these tests encode, continuing #42's D1-D3 (see
// tests/PresetIrReferenceTests.cpp) so a failure here reads as a product
// regression rather than a mechanical one:
//
//   D4  The embedded bytes ARE a resolution source. A preset that references
//       one of Nave's own cabinets resolves on a machine where nobody has
//       pressed "Install Library" - including a machine that has never seen
//       the preset's author's IR folder.
//
//   D5  The user's library comes FIRST, the embedded copy only after it. The
//       ordering picks which FILE the slot points at and never which SOUND
//       comes out, because a digest can only match bytes equal to it: when
//       both sources hold the hash they hold the same audio. There is
//       therefore no "the two sources disagree" case to surface.
//
//   D6  The bundle is a resolution source, NOT a library. It is keyed by
//       digest alone - never scanned, never listed, never offered - so the
//       only thing it can ever answer is an explicit reference to specific
//       bytes, and its cache folder is not the folder the browser scans.
//
//   D7  The lookup is TOTAL. Every digest lands on exactly one outcome
//       (notReferenced / alreadyLoaded / library / bundled / notFound), and
//       the not-found outcome performs NO audio operation at all: the slot
//       keeps the samples it had, which is why it cannot produce a NaN, a
//       full-scale click, or silence where there was signal.
//
//   D8  There is still exactly ONE decode path. The embedded bytes are
//       written to a real file and loaded through the same
//       loadImpulseResponseFromFile() a browser selection uses, so the two
//       resolution sources cannot drift into sounding different.
//
// NOTHING HERE IS WALL-CLOCK SENSITIVE. Where a test needs to prove the
// resolution reached the DSP it compares the buffer the convolution engine
// holds (NaveAudioProcessor::getLoadedImpulseResponse), which is set
// synchronously - unlike the audible result of a juce::dsp::Convolution
// load, which only appears once its background preparation completes and
// would need a sleep to observe.
namespace
{
    using basilica::ir::BundledIrSource;
    using basilica::ir::FactoryIrAsset;
    using basilica::presets::PresetManager;
    using basilica::presets::PresetManagerConfig;

    struct ScopedTestDirectory
    {
        explicit ScopedTestDirectory (const juce::String& label)
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("NaveBundledIrResolutionTests")
                       .getChildFile (label + "_"
                                       + juce::String (juce::Time::getHighResolutionTicks())
                                       + "_" + juce::String (juce::Random::getSystemRandom().nextInt (1000000))))
        {
            dir.createDirectory();
        }

        ~ScopedTestDirectory() { dir.deleteRecursively(); }

        JUCE_DECLARE_NON_COPYABLE (ScopedTestDirectory)

        juce::File dir;
    };

    const std::vector<FactoryIrAsset>& assets() { return nave::factoryIrAssets(); }

    const BundledIrSource& source()
    {
        static const BundledIrSource instance { assets() };
        return instance;
    }

    const FactoryIrAsset& assetNamed (const juce::String& fileName)
    {
        for (const auto& asset : assets())
            if (fileName == asset.fileName)
                return asset;

        FAIL ("no embedded asset named " << fileName.toStdString());
        return assets().front();
    }

    juce::String digestOf (const juce::String& fileName)
    {
        const auto digest = BundledIrSource::contentHashOf (assetNamed (fileName));
        REQUIRE (digest.length() == 64);
        return digest;
    }

    PresetManagerConfig makeIsolatedConfig (const juce::File& userPresetDir)
    {
        PresetManagerConfig config;
        config.pluginId = "com.yvesvogl.nave";
        config.pluginName = "Nave";
        config.manufacturerName = "Basilica Audio";
        config.pluginVersion = "0.5.0-test";
        config.userPresetsDirectoryOverrideForTests = userPresetDir;
        return config;
    }

    std::vector<basilica::presets::FactoryPresetAsset> factoryPresetAssets()
    {
        return {
            { BinaryData::default_json, BinaryData::default_jsonSize },
            { BinaryData::tameTheFizz_json, BinaryData::tameTheFizz_jsonSize },
            { BinaryData::liveStage_json, BinaryData::liveStage_jsonSize },
            { BinaryData::darkVintage_json, BinaryData::darkVintage_jsonSize },
            { BinaryData::pushedBackInTheRoom_json, BinaryData::pushedBackInTheRoom_jsonSize },
            { BinaryData::touchOfRoomMic_json, BinaryData::touchOfRoomMic_jsonSize },
            { BinaryData::evenBlend_json, BinaryData::evenBlend_jsonSize },
            { BinaryData::parallelCabBlendedDry_json, BinaryData::parallelCabBlendedDry_jsonSize },
            { BinaryData::micMorph_json, BinaryData::micMorph_jsonSize },
            { BinaryData::tightStack_json, BinaryData::tightStack_jsonSize },
        };
    }

    // Every test that can reach the embedded source has to be told where to
    // write, or it would materialise into the developer's (and CI's) real
    // ~/Library|%APPDATA% location - the same reason the preset tests override
    // the user preset directory.
    void useFolders (NaveAudioProcessor& processor,
                     const juce::File& libraryFolder,
                     const juce::File& cacheFolder)
    {
        processor.apvts.state.setProperty (ParamIDs::irLibraryFolderProperty,
                                           libraryFolder.getFullPathName(), nullptr);
        processor.setBundledIrCacheDirectoryForTests (cacheFolder);
        processor.refreshIrSearchRoots();
    }

    bool buffersIdentical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
    {
        if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
            return false;

        for (int channel = 0; channel < a.getNumChannels(); ++channel)
            for (int sample = 0; sample < a.getNumSamples(); ++sample)
                if (a.getReadPointer (channel)[sample] != b.getReadPointer (channel)[sample])
                    return false;

        return true;
    }

    juce::AudioBuffer<float> readWav (const juce::File& file)
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();

        const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        REQUIRE (reader != nullptr);

        juce::AudioBuffer<float> buffer (juce::jlimit (1, 2, static_cast<int> (reader->numChannels)),
                                          static_cast<int> (reader->lengthInSamples));
        reader->read (&buffer, 0, buffer.getNumSamples(), 0, true, true);
        return buffer;
    }

    bool fileHoldsAssetBytes (const juce::File& file, const FactoryIrAsset& asset)
    {
        juce::MemoryBlock onDisk;

        if (! file.loadFileAsData (onDisk))
            return false;

        return onDisk.getSize() == static_cast<size_t> (asset.dataSize)
            && std::memcmp (onDisk.getData(), asset.data, onDisk.getSize()) == 0;
    }

    juce::String makePresetFile (const juce::String& name, const juce::String& irJson)
    {
        juce::String text;
        text << "{\n";
        text << "  \"format\": \"basilica-preset-1\",\n";
        text << "  \"plugin\": \"com.yvesvogl.nave\",\n";
        text << "  \"pluginVersion\": \"0.5.0\",\n";
        text << "  \"stateVersion\": 2,\n";
        text << "  \"name\": \"" << name << "\",\n";
        text << "  \"category\": \"Guitar\",\n";
        text << "  \"ir\": " << irJson << ",\n";
        text << "  \"parameters\": " << R"({ "loCut": 90.0, "hiCut": 6200.0, "mix": 72.0, "level": -1.5,
             "irBlend": 40.0, "micDistance": 30.0, "blendMode": 1.0,
             "alignMode": 1.0, "irBTrim": -2.0, "irBPolarity": 0.0,
             "irBDelay": 0.25, "irGainMode": 1.0, "irAMinPhase": 0.0,
             "irBMinPhase": 0.0, "distanceAir": 1.0, "loCutSlope": 1.0,
             "hiCutSlope": 0.0 })" << "\n";
        text << "}\n";
        return text;
    }

    juce::String irJsonFor (const juce::String& hashA, const juce::String& nameA,
                             const juce::String& hashB = {}, const juce::String& nameB = {})
    {
        juce::String text;
        text << "{ \"a\": { \"sha256\": \"" << hashA << "\", \"name\": \"" << nameA << "\" }";

        if (hashB.isNotEmpty())
            text << ", \"b\": { \"sha256\": \"" << hashB << "\", \"name\": \"" << nameB << "\" }";

        text << " }";
        return text;
    }

    // Writes a preset carrying `irJson` into `presetDir` and loads it through
    // the real production path.
    void loadPresetWithReference (NaveAudioProcessor& processor,
                                  PresetManager& manager,
                                  const juce::File& presetDir,
                                  const juce::String& name,
                                  const juce::String& irJson)
    {
        juce::ignoreUnused (processor);

        const auto file = presetDir.getChildFile (name + PresetManager::presetFileExtension);
        REQUIRE (file.replaceWithText (makePresetFile (name, irJson)));
        REQUIRE (manager.loadPreset (name));
    }

    // A copy of one embedded asset written under an arbitrary name, so a test
    // can put the bundled BYTES in the library without the bundled NAME.
    juce::File copyAssetInto (const juce::File& folder,
                              const FactoryIrAsset& asset,
                              const juce::String& fileName)
    {
        const auto file = folder.getChildFile (fileName);
        REQUIRE (file.replaceWithData (asset.data, static_cast<size_t> (asset.dataSize)));
        return file;
    }
}

//==============================================================================
// D6 - the bundle is a resolution source, not a library.

TEST_CASE ("Bundled IR resolution: the embedded source indexes cabinets and nothing else",
           "[ir][bundled][content]")
{
    // Nine cabinets, and only the cabinets. The provenance files travel with
    // the library (issue #33's licensing bar is a licence committed ALONGSIDE
    // the audio), so they are in the same asset list - and a preset must not
    // be able to "resolve" its IR slot to a Markdown file.
    CHECK (source().allContentHashes().size() == 9);

    for (const char* provenanceFile : { "LICENSES.md", "CC0-1.0.txt", "manifest.json" })
    {
        const auto& asset = assetNamed (provenanceFile);
        const auto digest = BundledIrSource::contentHashOf (asset);

        REQUIRE (digest.length() == 64); // the bytes are there...
        CHECK (source().findByContentHash (digest) == nullptr); // ...but unreachable as an IR
    }

    for (const auto& asset : assets())
    {
        if (! BundledIrSource::isAudioAssetName (asset.fileName))
            continue;

        const auto digest = BundledIrSource::contentHashOf (asset);
        const auto* found = source().findByContentHash (digest);

        REQUIRE (found != nullptr);
        CHECK (juce::String (found->fileName) == juce::String (asset.fileName));
    }
}

TEST_CASE ("Bundled IR resolution: the cache folder is not the library folder and is never scanned",
           "[ir][bundled]")
{
    const auto library = basilica::ir::IrLibrary::defaultDirectory();
    const auto cache = basilica::ir::IrLibrary::bundledCacheDirectory();

    // Writing the embedded copy into the folder the browser scans would be
    // the silent install src/ir/FactoryIrLibrary.h refuses to perform, and
    // would put files the user never chose into their own library listing.
    CHECK (cache != library);
    CHECK_FALSE (cache.isAChildOf (library));
    CHECK_FALSE (library.isAChildOf (cache));

    // Neither is it a search root: the cache answers explicit digests only.
    NaveAudioProcessor processor;

    for (const auto& root : processor.getIrSearchRoots())
        CHECK (root != cache);
}

TEST_CASE ("Bundled IR resolution: a malformed digest is a miss in the embedded source too",
           "[ir][bundled]")
{
    for (const char* bad : { "", "   ", "not-a-hash", "abc",
                              "ZZZZ567890123456789012345678901234567890123456789012345678901234" })
        CHECK (source().findByContentHash (bad) == nullptr);

    // Well-formed but nobody's.
    CHECK (source().findByContentHash (juce::String::repeatedString ("ab", 32)) == nullptr);

    // Case and surrounding whitespace must not change the answer - a digest
    // pasted out of resources/irs/manifest.json by hand still has to resolve.
    const auto digest = digestOf ("modelled_4x12_ceramic_cone.wav");
    CHECK (source().findByContentHash (digest.toUpperCase()) != nullptr);
    CHECK (source().findByContentHash ("  " + digest + "  ") != nullptr);
}

//==============================================================================
// D8 - materialising, and the single decode path.

TEST_CASE ("Bundled IR resolution: materialising writes the embedded bytes exactly, and is idempotent",
           "[ir][bundled][content]")
{
    ScopedTestDirectory cache ("materialise");

    const auto& asset = assetNamed ("modelled_4x12_ceramic_cone.wav");

    const auto first = source().materialise (asset, cache.dir);
    REQUIRE (first.existsAsFile());
    CHECK (first.getFileName() == juce::String (asset.fileName));
    CHECK (fileHoldsAssetBytes (first, asset));

    // Idempotent: asked again, the same file comes back, still byte-exact.
    // Asserted on CONTENT rather than on a modification timestamp, which
    // would be a wall-clock assumption.
    const auto second = source().materialise (asset, cache.dir);
    CHECK (second == first);
    CHECK (fileHoldsAssetBytes (second, asset));

    // ...and it repairs rather than trusting what it finds: a file of the
    // right name holding the wrong bytes is exactly the case where handing it
    // to the convolver would be the silent-wrong-sound failure.
    REQUIRE (first.replaceWithText ("not audio at all"));
    CHECK_FALSE (fileHoldsAssetBytes (first, asset));

    const auto repaired = source().materialise (asset, cache.dir);
    CHECK (repaired == first);
    CHECK (fileHoldsAssetBytes (repaired, asset));
}

TEST_CASE ("Bundled IR resolution: every bundled cabinet resolves to its own bytes",
           "[ir][bundled][content]")
{
    ScopedTestDirectory cache ("all-cabs");
    ScopedTestDirectory emptyLibrary ("all-cabs-library");

    NaveAudioProcessor processor;
    useFolders (processor, emptyLibrary.dir, cache.dir);

    int cabinets = 0;

    for (const auto& asset : assets())
    {
        if (! BundledIrSource::isAudioAssetName (asset.fileName))
            continue;

        ++cabinets;

        const auto resolved = processor.resolveIrReference (BundledIrSource::contentHashOf (asset), {});

        INFO ("asset: " << asset.fileName);
        CHECK (resolved.source == NaveAudioProcessor::IrReferenceSource::bundled);
        REQUIRE (resolved.file.existsAsFile());
        CHECK (resolved.file.getFileName() == juce::String (asset.fileName));
        CHECK (fileHoldsAssetBytes (resolved.file, asset));
    }

    CHECK (cabinets == 9);
}

//==============================================================================
// D7 - totality.

TEST_CASE ("Bundled IR resolution: every lookup has exactly one defined outcome",
           "[ir][bundled][presets]")
{
    using Source = NaveAudioProcessor::IrReferenceSource;

    ScopedTestDirectory cache ("total-cache");
    ScopedTestDirectory library ("total-library");

    NaveAudioProcessor processor;
    useFolders (processor, library.dir, cache.dir);

    const auto bundledDigest = digestOf ("modelled_8x10_cone.wav");

    // A user IR that is NOT one of Nave's: the bytes of a bundled cabinet
    // would resolve from the bundle even when the file was deleted, which
    // would make "the library found it" untestable.
    juce::AudioBuffer<float> userIr (1, 128);
    userIr.clear();
    userIr.setSample (0, 0, 1.0f);
    userIr.setSample (0, 41, -0.5f);

    const auto userFile = library.dir.getChildFile ("user_cab.wav");
    REQUIRE (TestHelpers::writeWavFile (userFile, userIr, 48000.0));
    const auto userDigest = basilica::presets::contentHashOfFile (userFile);
    REQUIRE (userDigest.length() == 64);

    SECTION ("no digest at all is not a reference")
    {
        for (const char* empty : { "", "   " })
        {
            const auto resolved = processor.resolveIrReference (empty, {});
            CHECK (resolved.source == Source::notReferenced);
            CHECK (resolved.file == juce::File());
        }
    }

    SECTION ("a digest that is not a digest is a miss, not a non-reference")
    {
        // The preset MEANT to name an IR and failed to. Reporting that as
        // "no reference" would swallow a corrupt preset silently.
        for (const char* malformed : { "abc", "not-a-hash",
                                        "ZZZZ567890123456789012345678901234567890123456789012345678901234" })
        {
            const auto resolved = processor.resolveIrReference (malformed, {});
            CHECK (resolved.source == Source::notFound);
            CHECK (resolved.file == juce::File());
        }
    }

    SECTION ("a well-formed digest nobody holds is a miss")
    {
        const auto resolved = processor.resolveIrReference (juce::String::repeatedString ("cd", 32), {});
        CHECK (resolved.source == Source::notFound);
        CHECK (resolved.file == juce::File());
    }

    SECTION ("a digest the library holds resolves from the library")
    {
        const auto resolved = processor.resolveIrReference (userDigest, {});
        CHECK (resolved.source == Source::library);
        CHECK (resolved.file == userFile);
    }

    SECTION ("a digest only the bundle holds resolves from the bundle")
    {
        const auto resolved = processor.resolveIrReference (bundledDigest, {});
        CHECK (resolved.source == Source::bundled);
        REQUIRE (resolved.file.existsAsFile());
        CHECK (resolved.file.isAChildOf (cache.dir));
    }

    SECTION ("a digest the slot already holds is answered without going anywhere")
    {
        const auto resolved = processor.resolveIrReference (userDigest, userFile.getFullPathName());
        CHECK (resolved.source == Source::alreadyLoaded);
        CHECK (resolved.file == userFile);

        // ...and specifically not by writing anything: the short-circuit is
        // ahead of both sources.
        CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
    }

    SECTION ("a stale current path does not short-circuit anything")
    {
        // The slot names a file that has since been deleted. The digest still
        // has to be resolved from a source rather than trusted.
        const auto vanished = library.dir.getChildFile ("gone.wav");
        const auto resolved = processor.resolveIrReference (userDigest, vanished.getFullPathName());
        CHECK (resolved.source == Source::library);
        CHECK (resolved.file == userFile);
    }
}

TEST_CASE ("Bundled IR resolution: a cache folder that cannot be written degrades to a miss",
           "[ir][bundled][presets]")
{
    ScopedTestDirectory scratch ("unwritable");
    ScopedTestDirectory presetDir ("unwritable-presets");
    ScopedTestDirectory emptyLibrary ("unwritable-library");

    // A cache path whose parent is a regular FILE, so createDirectory() must
    // fail. Cheaper and more portable than manipulating permissions, and it
    // exercises exactly the branch that matters: the embedded copy is there,
    // and it still cannot become a file.
    const auto blocker = scratch.dir.getChildFile ("blocker");
    REQUIRE (blocker.replaceWithText ("not a directory"));

    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, emptyLibrary.dir, blocker.getChildFile ("cache"));

    const auto bundledDigest = digestOf ("modelled_4x12_ceramic_cone.wav");

    const auto resolved = processor.resolveIrReference (bundledDigest, {});
    CHECK (resolved.source == NaveAudioProcessor::IrReferenceSource::notFound);
    CHECK (resolved.file == juce::File());

    // Through the real preset path: the preset still opens, the slot is
    // untouched, and the notice says so.
    juce::AudioBuffer<float> irBefore;
    irBefore.makeCopyOf (processor.getLoadedImpulseResponse (0));

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (processor, manager, presetDir.dir, "Unwritable",
                             irJsonFor (bundledDigest, "Modelled 4x12 Ceramic Cone"));

    CHECK (processor.getCurrentIrFilePath().isEmpty());
    CHECK (buffersIdentical (processor.getLoadedImpulseResponse (0), irBefore));

    const auto notice = processor.getPresetIrNotice();
    INFO ("notice: " << notice.toStdString());
    CHECK (notice.contains ("Modelled 4x12 Ceramic Cone"));
    CHECK (notice.contains ("Install Library"));
}

TEST_CASE ("Bundled IR resolution: a miss performs no audio operation at all",
           "[ir][bundled][presets][dsp]")
{
    ScopedTestDirectory cache ("miss-cache");
    ScopedTestDirectory presetDir ("miss-presets");
    ScopedTestDirectory library ("miss-library");

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    NaveAudioProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    useFolders (processor, library.dir, cache.dir);

    // A real, audible IR in the slot first. This is what "degrade audibly
    // safely" is actually about: the player has a cabinet up, opens a preset
    // that names one they do not have, and whatever was playing must go on
    // playing - not turn into silence, a NaN or a discontinuity.
    juce::AudioBuffer<float> playingIr (1, 192);
    playingIr.clear();
    playingIr.setSample (0, 0, 0.9f);
    playingIr.setSample (0, 23, -0.4f);
    playingIr.setSample (0, 64, 0.2f);

    const auto playing = library.dir.getChildFile ("playing.wav");
    REQUIRE (TestHelpers::writeWavFile (playing, playingIr, sampleRate));
    REQUIRE (processor.loadImpulseResponseFromFile (playing));

    const auto checkIrIsUsable = [&processor] (const char* when)
    {
        const auto& ir = processor.getLoadedImpulseResponse (0);
        INFO (when);
        REQUIRE (ir.getNumSamples() > 0);

        float peak = 0.0f;

        for (int channel = 0; channel < ir.getNumChannels(); ++channel)
            for (int sample = 0; sample < ir.getNumSamples(); ++sample)
            {
                const auto value = ir.getReadPointer (channel)[sample];
                REQUIRE (std::isfinite (value));
                peak = juce::jmax (peak, std::abs (value));
            }

        // Not silence: there is still an impulse response to convolve with.
        CHECK (peak > 0.0f);
    };

    checkIrIsUsable ("before the preset load");

    juce::AudioBuffer<float> irBefore;
    irBefore.makeCopyOf (processor.getLoadedImpulseResponse (0));

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (processor, manager, presetDir.dir, "Nobody Has This",
                             irJsonFor (juce::String::repeatedString ("ef", 32), "A Cab Nobody Owns"));

    // Bit-identical to what it was. That is the whole safety argument: an
    // operation that does not happen cannot produce a NaN, a discontinuity or
    // a dropout.
    CHECK (buffersIdentical (processor.getLoadedImpulseResponse (0), irBefore));
    CHECK (processor.getCurrentIrFilePath() == playing.getFullPathName());
    checkIrIsUsable ("after the missing reference");

    CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
    CHECK (processor.getPresetIrNotice().isNotEmpty());

    // And the plugin still renders finite, in-range audio afterwards.
    //
    // The bound is 1.0 rather than a measured level: the claim is "no
    // full-scale click", not a gain figure. No assertion is made about how
    // LOUD the output is, deliberately - juce::dsp::Convolution swaps a new
    // IR in only after its own background preparation completes, so any
    // level-based expectation here would be a wall-clock assumption. The IR
    // checks above are the synchronous form of the same claim.
    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        TestHelpers::fillWithSine (audio, sampleRate, 440.0, 0.5f,
                                    static_cast<juce::int64> (block) * blockSize);
        processor.processBlock (audio, midi);

        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            {
                const auto value = audio.getReadPointer (channel)[sample];
                REQUIRE (std::isfinite (value));
                REQUIRE (std::abs (value) <= 1.0f);
            }
    }
}

TEST_CASE ("Bundled IR resolution: a miss on a slot that holds nothing leaves it holding nothing",
           "[ir][bundled][presets][dsp]")
{
    // The other half: a fresh instance, no user IR anywhere. The engine's
    // default state is the internal delta (unity) path, which is why
    // getLoadedImpulseResponse() reports an EMPTY raw buffer - there is no
    // user IR to report. A miss must leave exactly that, rather than
    // installing something or clearing something that was never there.
    ScopedTestDirectory cache ("miss-default-cache");
    ScopedTestDirectory presetDir ("miss-default-presets");
    ScopedTestDirectory library ("miss-default-library");

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 256;

    NaveAudioProcessor processor;
    processor.prepareToPlay (sampleRate, blockSize);
    useFolders (processor, library.dir, cache.dir);

    REQUIRE (processor.getCurrentIrFilePath().isEmpty());
    REQUIRE (processor.getLoadedImpulseResponse (0).getNumSamples() == 0);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (processor, manager, presetDir.dir, "Nobody Has This Either",
                             irJsonFor (juce::String::repeatedString ("ef", 32), "A Cab Nobody Owns"));

    CHECK (processor.getCurrentIrFilePath().isEmpty());
    CHECK (processor.getLoadedImpulseResponse (0).getNumSamples() == 0);
    CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
    CHECK (processor.getPresetIrNotice().isNotEmpty());

    juce::AudioBuffer<float> audio (2, blockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        TestHelpers::fillWithSine (audio, sampleRate, 440.0, 0.5f,
                                    static_cast<juce::int64> (block) * blockSize);
        processor.processBlock (audio, midi);

        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
            for (int sample = 0; sample < audio.getNumSamples(); ++sample)
            {
                const auto value = audio.getReadPointer (channel)[sample];
                REQUIRE (std::isfinite (value));
                REQUIRE (std::abs (value) <= 1.0f);
            }
    }
}

//==============================================================================
// D4 / D5 / D8 - through the real preset path.

TEST_CASE ("Bundled IR resolution: a factory preset resolves with no library installed",
           "[ir][bundled][presets][factory][content]")
{
    // The case #45 exists for: a first-run user, nothing in the IR folder,
    // opening a FACTORY preset that references FACTORY cabinets already
    // inside the binary they just installed.
    ScopedTestDirectory cache ("factory-cache");
    ScopedTestDirectory presetDir ("factory-presets");
    ScopedTestDirectory emptyLibrary ("factory-library");

    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, emptyLibrary.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    REQUIRE (manager.loadPreset ("Mic Morph"));

    // Nothing to report - the whole point.
    CHECK (processor.getPresetIrNotice().isEmpty());

    // Both slots hold the cabinets docs/presets.md prescribes for Mic Morph,
    // and hold them as SAMPLES, not merely as a path property.
    const auto& coneAsset = assetNamed ("modelled_4x12_ceramic_cone.wav");
    const auto& edgeAsset = assetNamed ("modelled_4x12_ceramic_edge.wav");

    const juce::File slotA (processor.getCurrentIrFilePath());
    const juce::File slotB (processor.getCurrentIrFilePathB());

    REQUIRE (slotA.existsAsFile());
    REQUIRE (slotB.existsAsFile());
    CHECK (slotA.isAChildOf (cache.dir));
    CHECK (slotB.isAChildOf (cache.dir));
    CHECK (fileHoldsAssetBytes (slotA, coneAsset));
    CHECK (fileHoldsAssetBytes (slotB, edgeAsset));

    CHECK (buffersIdentical (processor.getLoadedImpulseResponse (0), readWav (slotA)));
    CHECK (buffersIdentical (processor.getLoadedImpulseResponse (1), readWav (slotB)));

    // ...and they are genuinely different cabinets, so "both slots resolved"
    // could not have been passed by loading one file twice.
    CHECK_FALSE (buffersIdentical (processor.getLoadedImpulseResponse (0),
                                    processor.getLoadedImpulseResponse (1)));

    // The reference survives a re-save: the materialised file is a real file
    // holding the referenced bytes, so capturing the slot re-derives the same
    // digest. A memory-only load could not offer this.
    REQUIRE (manager.saveUserPreset ("Mic Morph Copy", "Guitar"));

    const auto saved = presetDir.dir.getChildFile (juce::String ("Mic Morph Copy") + PresetManager::presetFileExtension);
    REQUIRE (saved.existsAsFile());

    const auto text = saved.loadFileAsString();
    CHECK (text.contains (BundledIrSource::contentHashOf (coneAsset)));
    CHECK (text.contains (BundledIrSource::contentHashOf (edgeAsset)));
}

TEST_CASE ("Bundled IR resolution: the installed library wins over the embedded copy",
           "[ir][bundled][presets][factory][content]")
{
    // D5. Both sources hold the digest, because "Install Library" writes
    // exactly the embedded bytes - so this is a test about WHICH FILE, and
    // the accompanying sample comparison is what shows the choice is not
    // audible.
    ScopedTestDirectory cache ("precedence-cache");
    ScopedTestDirectory presetDir ("precedence-presets");
    ScopedTestDirectory library ("precedence-library");

    const auto install = basilica::ir::FactoryIrLibrary::installInto (library.dir, assets());
    REQUIRE (install.succeeded());

    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, library.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    REQUIRE (manager.loadPreset ("Mic Morph"));

    const juce::File slotA (processor.getCurrentIrFilePath());
    REQUIRE (slotA.existsAsFile());
    CHECK (slotA.isAChildOf (library.dir));
    CHECK_FALSE (slotA.isAChildOf (cache.dir));

    // Nothing was materialised at all: the embedded source was never reached.
    CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);

    CHECK (processor.getPresetIrNotice().isEmpty());
}

TEST_CASE ("Bundled IR resolution: both sources put sample-identical audio into the engine",
           "[ir][bundled][presets][factory][content][dsp]")
{
    // D8. Free today, because the embedded path materialises a file and then
    // uses the ONE decode path. Pinned anyway: the day somebody adds a
    // memory decoder to save a write is the day it stops being free, and a
    // preset that sounds different depending on whether the library happens
    // to be installed is exactly the failure #45 weighed.
    ScopedTestDirectory libraryDir ("identical-library");
    ScopedTestDirectory installedCache ("identical-cache-a");
    ScopedTestDirectory bundledCache ("identical-cache-b");
    ScopedTestDirectory emptyLibrary ("identical-empty");
    ScopedTestDirectory presetA ("identical-presets-a");
    ScopedTestDirectory presetB ("identical-presets-b");

    REQUIRE (basilica::ir::FactoryIrLibrary::installInto (libraryDir.dir, assets()).succeeded());

    const auto renderFrom = [] (const juce::File& library,
                                 const juce::File& cache,
                                 const juce::File& presets,
                                 juce::AudioBuffer<float>& slotAOut,
                                 juce::AudioBuffer<float>& slotBOut,
                                 double& slotARateOut)
    {
        NaveAudioProcessor processor;
        processor.prepareToPlay (48000.0, 512);
        useFolders (processor, library, cache);

        PresetManager manager (processor.apvts, makeIsolatedConfig (presets), factoryPresetAssets());
        processor.installPresetIrCallbacks (manager);

        REQUIRE (manager.loadPreset ("Even Blend"));
        REQUIRE (processor.getPresetIrNotice().isEmpty());

        slotAOut.makeCopyOf (processor.getLoadedImpulseResponse (0));
        slotBOut.makeCopyOf (processor.getLoadedImpulseResponse (1));
        slotARateOut = processor.getLoadedImpulseResponseSampleRate (0);
    };

    juce::AudioBuffer<float> installedA, installedB, bundledA, bundledB;
    double installedRate = 0.0, bundledRate = 0.0;

    renderFrom (libraryDir.dir, installedCache.dir, presetA.dir, installedA, installedB, installedRate);
    renderFrom (emptyLibrary.dir, bundledCache.dir, presetB.dir, bundledA, bundledB, bundledRate);

    // The first run must genuinely have come from the library and the second
    // genuinely from the bundle, or this compares one path with itself.
    CHECK (installedCache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
    CHECK (bundledCache.dir.getNumberOfChildFiles (juce::File::findFiles) > 0);

    REQUIRE (installedA.getNumSamples() > 0);
    REQUIRE (installedB.getNumSamples() > 0);
    CHECK (buffersIdentical (installedA, bundledA));
    CHECK (buffersIdentical (installedB, bundledB));
    CHECK (installedRate == bundledRate);
    CHECK (installedRate == Catch::Approx (48000.0));
}

//==============================================================================
// D5 / D6 - shadowing.

TEST_CASE ("Bundled IR resolution: a bundled cabinet cannot be shadowed by name",
           "[ir][bundled][presets]")
{
    ScopedTestDirectory cache ("shadow-cache");
    ScopedTestDirectory presetDir ("shadow-presets");
    ScopedTestDirectory library ("shadow-library");

    const auto& coneAsset = assetNamed ("modelled_4x12_ceramic_cone.wav");
    const auto coneDigest = BundledIrSource::contentHashOf (coneAsset);

    // An impostor: Nave's own file NAME, somebody else's audio. Nothing
    // resolves by name, so this must not be able to stand in for the
    // reference - that would be the silent-wrong-sound failure #42 chose a
    // content hash to prevent.
    juce::AudioBuffer<float> impostorIr (1, 96);
    impostorIr.clear();
    impostorIr.setSample (0, 0, 1.0f);
    impostorIr.setSample (0, 17, 0.8f);

    const auto impostor = library.dir.getChildFile ("modelled_4x12_ceramic_cone.wav");
    REQUIRE (TestHelpers::writeWavFile (impostor, impostorIr, 48000.0));

    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, library.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (processor, manager, presetDir.dir, "Shadowed",
                             irJsonFor (coneDigest, "Modelled 4x12 Ceramic Cone"));

    const juce::File slotA (processor.getCurrentIrFilePath());
    REQUIRE (slotA.existsAsFile());
    CHECK (slotA.isAChildOf (cache.dir));
    CHECK (fileHoldsAssetBytes (slotA, coneAsset));
    CHECK_FALSE (buffersIdentical (processor.getLoadedImpulseResponse (0), impostorIr));
    CHECK (processor.getPresetIrNotice().isEmpty());
}

TEST_CASE ("Bundled IR resolution: a user's own copy of a bundled cabinet is used, whatever it is called",
           "[ir][bundled][presets]")
{
    // The other half of D5: the bytes are what is referenced, so a user who
    // keeps Nave's cabinets under their own names in their own folder gets
    // THEIR file - the one they can see and audition - rather than a copy
    // appearing somewhere they did not choose.
    ScopedTestDirectory cache ("rename-cache");
    ScopedTestDirectory presetDir ("rename-presets");
    ScopedTestDirectory library ("rename-library");

    const auto& hornAsset = assetNamed ("modelled_4x10_horn.wav");
    const auto renamed = copyAssetInto (library.dir, hornAsset, "My Favourite Cab.wav");

    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, library.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (processor, manager, presetDir.dir, "Renamed",
                             irJsonFor (BundledIrSource::contentHashOf (hornAsset), "Modelled 4x10 Horn"));

    CHECK (processor.getCurrentIrFilePath() == renamed.getFullPathName());
    CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);
    CHECK (processor.getPresetIrNotice().isEmpty());
}

TEST_CASE ("Bundled IR resolution: a retuned cabinet still misses loudly",
           "[ir][bundled][presets]")
{
    // The property #42 chose a byte hash for, restated against the new
    // source: if a model is ever retuned, its bytes change, and a preset made
    // against the OLD bytes must not silently recall the new sound. The
    // embedded source is keyed by the same digest, so it cannot rescue a
    // stale reference either - which is the correct outcome, not a gap.
    ScopedTestDirectory cache ("retune-cache");
    ScopedTestDirectory presetDir ("retune-presets");
    ScopedTestDirectory library ("retune-library");

    const auto& coneAsset = assetNamed ("modelled_4x12_ceramic_cone.wav");

    // A digest one nibble away from a real bundled cabinet: what a retune
    // would look like from a preset's point of view.
    auto retunedDigest = BundledIrSource::contentHashOf (coneAsset);
    retunedDigest = retunedDigest.substring (0, 63)
                  + (retunedDigest.getLastCharacter() == '0' ? "1" : "0");

    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    useFolders (processor, library.dir, cache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    loadPresetWithReference (processor, manager, presetDir.dir, "Retuned",
                             irJsonFor (retunedDigest, "Modelled 4x12 Ceramic Cone (v1)"));

    CHECK (processor.getCurrentIrFilePath().isEmpty());
    CHECK (cache.dir.getNumberOfChildFiles (juce::File::findFiles) == 0);

    const auto notice = processor.getPresetIrNotice();
    INFO ("notice: " << notice.toStdString());
    CHECK (notice.contains ("Modelled 4x12 Ceramic Cone (v1)"));

    // ...and it is NOT reported as one of Nave's own, because that digest is
    // not one of Nave's own - the hint would be a lie.
    CHECK_FALSE (notice.contains ("Install Library"));
}
