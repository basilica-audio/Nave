#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "ir/FactoryIrLibrary.h"
#include "ir/IrLibrary.h"

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <memory>

// Installing the bundled IR library (issue #33), measured rather than assumed.
//
// tests/FactoryIrLibraryTests.cpp proves the files in resources/irs/ are sound
// as signals and load through the engine. These cases cover the other half:
// that the copy EMBEDDED in the binary is the same audio, and that the
// browser's "Install Library" action puts it somewhere the browser's own
// directory scanner can then find it.
//
// The distinction matters because the two copies can drift silently. A file
// renamed in resources/irs/ but not in CMakeLists.txt, or an asset added to
// one list and not the other, produces a plugin that installs a library which
// no longer matches the SHA-256 manifest committed next to it - i.e. exactly
// the provenance claim #33 exists to protect, broken without anything failing.
//
// NOTHING HERE WRITES TO THE REAL LIBRARY FOLDER. Every install targets a
// temporary directory; IrLibrary::defaultDirectory() (~/Music/Nave/Impulse
// Responses) is never a destination in the test suite, only in the editor's
// button handler.
//
// NAVE_IR_ASSET_DIR is set by CMakeLists.txt to the in-repo asset directory.
namespace
{
    constexpr int expectedAudioAssets = 9;
    constexpr int expectedProvenanceAssets = 3; // LICENSES.md, CC0-1.0.txt, manifest.json

    juce::File sourceAssetDirectory()
    {
        return juce::File (juce::String (NAVE_IR_ASSET_DIR));
    }

    struct TempDestination
    {
        TempDestination()
            : folder (juce::File::getSpecialLocation (juce::File::tempDirectory)
                          .getChildFile ("nave-factory-ir-install")
                          .getNonexistentSibling())
        {
        }

        ~TempDestination() { folder.deleteRecursively(); }

        juce::File folder;
    };

    const basilica::ir::FactoryIrAsset& assetNamed (const juce::String& fileName)
    {
        for (const auto& asset : nave::factoryIrAssets())
            if (fileName == asset.fileName)
                return asset;

        FAIL ("no bundled asset named " << fileName);
        return nave::factoryIrAssets().front();
    }

    juce::MemoryBlock contentsOf (const juce::File& file)
    {
        juce::MemoryBlock block;
        REQUIRE (file.loadFileAsData (block));
        return block;
    }
}

//==============================================================================
TEST_CASE ("Embedded factory IR assets are byte-identical to the verified files in resources/irs",
           "[ir][factory][content][install]")
{
    // The load-bearing anti-drift check. resources/irs/manifest.json records a
    // SHA-256 per file, tools/ir-synth/verify_irs.py re-measures those files in
    // CI, and tools/ir-synth/cabsynth.py regenerates them byte-identically -
    // but all three vouch for the files ON DISK IN THE REPOSITORY. What the
    // plugin actually installs is the BinaryData copy, and nothing else in the
    // suite compares the two.
    const auto& assets = nave::factoryIrAssets();
    REQUIRE ((int) assets.size() == expectedAudioAssets + expectedProvenanceAssets);

    const auto sourceDirectory = sourceAssetDirectory();
    INFO ("source assets: " << sourceDirectory.getFullPathName());
    REQUIRE (sourceDirectory.isDirectory());

    for (const auto& asset : assets)
    {
        REQUIRE (asset.fileName != nullptr);
        INFO ("asset: " << asset.fileName);

        REQUIRE (asset.data != nullptr);
        REQUIRE (asset.dataSize > 0);

        const auto sourceFile = sourceDirectory.getChildFile (asset.fileName);
        REQUIRE (sourceFile.existsAsFile());

        const auto sourceBytes = contentsOf (sourceFile);
        REQUIRE (sourceBytes.getSize() == (size_t) asset.dataSize);
        CHECK (std::memcmp (sourceBytes.getData(), asset.data, (size_t) asset.dataSize) == 0);
    }
}

TEST_CASE ("The bundled library ships nine cabinets and its own provenance",
           "[ir][factory][content][install][licence]")
{
    // #33's licensing bar is a licence file committed ALONGSIDE the audio. An
    // install that unpacked only the WAVs would put a user's copy of the
    // library on disk with no statement of what it is or where it came from,
    // which is the condition the bar exists to prevent - so the provenance
    // files are part of the installed set, and that is asserted rather than
    // left to whoever next edits the list.
    int audioAssets = 0;
    juce::StringArray provenanceNames;

    for (const auto& asset : nave::factoryIrAssets())
    {
        const juce::File named { juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile (asset.fileName) };

        if (basilica::ir::IrLibrary::isImpulseResponseFile (named))
        {
            ++audioAssets;

            // Every audio asset is labelled as a model, not a capture - the
            // same binding condition tests/FactoryIrLibraryTests.cpp pins for
            // the repository copy, re-pinned here for the shipped list.
            CHECK (juce::String (asset.fileName).startsWith ("modelled_"));
        }
        else
        {
            provenanceNames.add (asset.fileName);
        }
    }

    CHECK (audioAssets == expectedAudioAssets);

    for (const auto* expected : { "LICENSES.md", "CC0-1.0.txt", "manifest.json" })
    {
        INFO ("provenance asset: " << expected);
        CHECK (provenanceNames.contains (expected));
    }
}

TEST_CASE ("Installing writes the whole library into an empty folder, and the browser's scanner finds it",
           "[ir][factory][install]")
{
    TempDestination destination;

    // Nothing exists yet - not even the folder, which is the real first-run
    // case (IrLibrary::defaultDirectory() deliberately creates nothing).
    REQUIRE_FALSE (destination.folder.exists());
    CHECK_FALSE (basilica::ir::FactoryIrLibrary::isInstalledIn (destination.folder, nave::factoryIrAssets()));

    const auto result = basilica::ir::FactoryIrLibrary::installInto (destination.folder, nave::factoryIrAssets());

    INFO ("install summary: " << result.summary());
    REQUIRE (result.succeeded());
    CHECK (result.written == expectedAudioAssets + expectedProvenanceAssets);
    CHECK (result.alreadyPresent == 0);

    for (const auto& asset : nave::factoryIrAssets())
    {
        INFO ("installed asset: " << asset.fileName);
        const auto installed = destination.folder.getChildFile (asset.fileName);
        REQUIRE (installed.existsAsFile());

        const auto installedBytes = contentsOf (installed);
        REQUIRE (installedBytes.getSize() == (size_t) asset.dataSize);
        CHECK (std::memcmp (installedBytes.getData(), asset.data, (size_t) asset.dataSize) == 0);
    }

    CHECK (basilica::ir::FactoryIrLibrary::isInstalledIn (destination.folder, nave::factoryIrAssets()));

    // The point of installing at all: the browser lists whatever
    // IrLibrary::scan() returns, so scanning the destination with the
    // browser's own scanner is what proves the library became browsable
    // rather than merely landing on disk. The three provenance files are not
    // audio, so they must NOT appear as cabinets.
    const auto scanned = basilica::ir::IrLibrary::scan (destination.folder);
    CHECK (scanned.size() == expectedAudioAssets);

    for (const auto& file : scanned)
    {
        INFO ("scanned: " << file.getFileName());
        CHECK (file.getFileName().startsWith ("modelled_"));
        CHECK (file.getFileName().endsWith (".wav"));
    }
}

TEST_CASE ("Installing twice writes nothing the second time", "[ir][factory][install]")
{
    TempDestination destination;

    const auto first = basilica::ir::FactoryIrLibrary::installInto (destination.folder, nave::factoryIrAssets());
    REQUIRE (first.succeeded());
    REQUIRE (first.written > 0);

    // Idempotence is not cosmetic here: the install is reachable from a button
    // a user can press repeatedly, and rewriting files that are already
    // correct would churn timestamps (and, on a slow or networked Music
    // folder, cost real time) for no change in outcome.
    const auto second = basilica::ir::FactoryIrLibrary::installInto (destination.folder, nave::factoryIrAssets());

    INFO ("second install summary: " << second.summary());
    CHECK (second.succeeded());
    CHECK (second.written == 0);
    CHECK (second.alreadyPresent == (int) nave::factoryIrAssets().size());
    CHECK (second.summary() == juce::String ("The bundled library is already installed"));
}

TEST_CASE ("A damaged or missing file re-offers the install, and installing repairs it",
           "[ir][factory][install]")
{
    TempDestination destination;

    REQUIRE (basilica::ir::FactoryIrLibrary::installInto (destination.folder, nave::factoryIrAssets()).succeeded());
    REQUIRE (basilica::ir::FactoryIrLibrary::isInstalledIn (destination.folder, nave::factoryIrAssets()));

    // Truncation, not deletion: a half-copied file is the case a plain
    // existence check gets wrong, and it is the one that would otherwise hand
    // the convolver a broken IR while the browser reported the library
    // present and correct.
    const auto victim = destination.folder.getChildFile ("modelled_4x12_ceramic_cone.wav");
    REQUIRE (victim.existsAsFile());
    REQUIRE (victim.replaceWithData ("not audio", 9));

    CHECK (basilica::ir::FactoryIrLibrary::stateOf (destination.folder,
                                                    assetNamed ("modelled_4x12_ceramic_cone.wav"))
           == basilica::ir::FactoryIrLibrary::AssetState::differs);
    CHECK_FALSE (basilica::ir::FactoryIrLibrary::isInstalledIn (destination.folder, nave::factoryIrAssets()));

    // And an outright missing file, so the repair covers both ways a library
    // folder degrades between visits.
    const auto deleted = destination.folder.getChildFile ("modelled_8x10_edge.wav");
    REQUIRE (deleted.deleteFile());
    CHECK (basilica::ir::FactoryIrLibrary::stateOf (destination.folder,
                                                    assetNamed ("modelled_8x10_edge.wav"))
           == basilica::ir::FactoryIrLibrary::AssetState::missing);

    const auto repair = basilica::ir::FactoryIrLibrary::installInto (destination.folder, nave::factoryIrAssets());

    INFO ("repair summary: " << repair.summary());
    CHECK (repair.succeeded());
    CHECK (repair.written == 2);
    CHECK (repair.alreadyPresent == (int) nave::factoryIrAssets().size() - 2);
    CHECK (basilica::ir::FactoryIrLibrary::isInstalledIn (destination.folder, nave::factoryIrAssets()));
}

TEST_CASE ("An installed cabinet loads through the convolution engine and changes the signal",
           "[ir][factory][install][content]")
{
    TempDestination destination;
    REQUIRE (basilica::ir::FactoryIrLibrary::installInto (destination.folder, nave::factoryIrAssets()).succeeded());

    // Scanned the way the browser scans, then loaded through the same
    // message-thread entry point the browser's row selection calls - so this
    // renders from the INSTALLED bytes rather than from resources/irs/.
    // tests/FactoryIrLibraryTests.cpp already walks all nine from the
    // repository copy, and the byte-identity case above ties the two together,
    // so one cabinet is enough here to prove the installed path reaches the
    // convolver.
    const auto scanned = basilica::ir::IrLibrary::scan (destination.folder);
    REQUIRE (scanned.size() == expectedAudioAssets);

    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    const auto renderWith = [] (const juce::File& irFile)
    {
        NaveAudioProcessor processor;
        processor.setPlayConfigDetails (2, 2, sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);

        if (irFile.existsAsFile())
            REQUIRE (processor.loadImpulseResponseFromFile (irFile));

        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::AudioBuffer<float> tail (2, blockSize);

        // juce::dsp::Convolution (JUCE 8.0.14) prepares a newly loaded IR on
        // its own background thread and swaps it in on a later process() call,
        // so this waits on wall-clock time rather than spinning - the same
        // cadence tests/FactoryIrLibraryTests.cpp uses.
        for (int block = 0; block < 24; ++block)
        {
            juce::Thread::sleep (15);

            TestHelpers::fillWithSine (buffer, sampleRate, 900.0, 0.5f, block * blockSize);
            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
            tail.makeCopyOf (buffer);
        }

        return tail;
    };

    const auto cabinet = destination.folder.getChildFile ("modelled_4x12_ceramic_cone.wav");
    REQUIRE (scanned.contains (cabinet));

    const auto dry = renderWith (juce::File());
    const auto wet = renderWith (cabinet);

    const auto dryRms = TestHelpers::rms (dry);
    REQUIRE (dryRms > 0.0);

    CHECK (TestHelpers::allSamplesFinite (wet));

    juce::AudioBuffer<float> difference;
    difference.makeCopyOf (wet);

    for (int channel = 0; channel < difference.getNumChannels(); ++channel)
        difference.addFrom (channel, 0, dry, channel, 0, difference.getNumSamples(), -1.0f);

    // A silently-failed load would leave the delta IR in place and render
    // identically to the dry path.
    CHECK (TestHelpers::rms (difference) > 0.05 * dryRms);
}
