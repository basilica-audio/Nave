#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "V1PresetLoaderReplica.h"
#include "ir/FactoryIrLibrary.h"
#include "ir/IrContentIndex.h"
#include "params/ParameterIds.h"
#include "presets/IrReference.h"
#include "presets/PresetManager.h"

#include <BinaryData.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

// Preset -> IR references (issue #42), and the compatibility contract the
// format change has to keep in BOTH directions.
//
// The decision these tests encode, restated so a failure here reads as a
// product regression rather than a mechanical one:
//
//   D1  The identifier is a hash of the IR file's BYTES. It is stable across a
//       regeneration of the bundled library (which is byte-identical) and it
//       MISSES after a retune, which degrades loudly instead of recalling a
//       different sound under the same name. The name travelling beside it is
//       for the message and nothing else - nothing resolves by name.
//   D2  A missing IR NEVER fails the preset. Parameters load, the currently
//       loaded IR stays exactly where it is, a notice names what was expected,
//       and nothing is substituted.
//   D3  Both directions. A v1 preset loads here; a v2 preset opens in a v1
//       build with its parameters intact, ignoring the field it cannot read.
//
// NOTHING HERE IS WALL-CLOCK SENSITIVE. Where a test has to prove that a
// reference reached the DSP rather than merely round-tripping through JSON, it
// compares the buffer the convolution engine actually holds
// (NaveAudioProcessor::getLoadedImpulseResponse) against the file's samples -
// a synchronous fact, unlike the audible result of a
// juce::dsp::Convolution load, which only appears after its background
// preparation completes.
namespace
{
    using basilica::presets::PresetManager;
    using basilica::presets::PresetManagerConfig;

    constexpr double irSampleRate = 48000.0;

    struct ScopedTestDirectory
    {
        explicit ScopedTestDirectory (const juce::String& label)
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("NavePresetIrReferenceTests")
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

    // Two impulse responses that are unmistakably different signals, so a
    // substitution of one for the other could never pass a sample comparison.
    juce::AudioBuffer<float> makeDistinctIr (int variant)
    {
        juce::AudioBuffer<float> buffer (1, 256);
        buffer.clear();

        auto* samples = buffer.getWritePointer (0);
        samples[0] = 1.0f;

        for (int i = 1; i < buffer.getNumSamples(); ++i)
            samples[i] = 0.45f * std::sin (juce::MathConstants<float>::twoPi
                                            * static_cast<float> ((variant + 1) * 7 * i) / 256.0f)
                          * std::exp (-static_cast<float> (i) / (18.0f * static_cast<float> (variant + 1)));

        return buffer;
    }

    juce::File writeIr (const juce::File& folder, const juce::String& fileName, int variant)
    {
        const auto file = folder.getChildFile (fileName);
        REQUIRE (TestHelpers::writeWavFile (file, makeDistinctIr (variant), irSampleRate));
        return file;
    }

    PresetManagerConfig makeIsolatedConfig (const juce::File& userPresetDir)
    {
        PresetManagerConfig config;
        config.pluginId = "com.yvesvogl.nave";
        config.pluginName = "Nave";
        config.manufacturerName = "Yves Vogl";
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

    // A single scratch location standing in for
    // IrLibrary::bundledCacheDirectory() (issue #45) for the whole test run,
    // deleted when the binary exits. No test may write into the user's real
    // ~/Library|%APPDATA%, and since #45 any preset naming one of Nave's own
    // digests would - so the redirect belongs in the shared helper rather
    // than being remembered per test.
    const juce::File& sharedBundledIrCache()
    {
        static const ScopedTestDirectory cache ("shared-bundled-cache");
        return cache.dir;
    }

    // Points the processor's reference resolution at a scratch library folder,
    // the same way a user choosing a folder in the IR browser does. A test
    // that cares WHERE the embedded copy lands overrides the cache again
    // after this call.
    void useIrLibraryFolder (NaveAudioProcessor& processor, const juce::File& folder)
    {
        processor.apvts.state.setProperty (ParamIDs::irLibraryFolderProperty, folder.getFullPathName(), nullptr);
        processor.setBundledIrCacheDirectoryForTests (sharedBundledIrCache());
        processor.refreshIrSearchRoots();
    }

    // Assembled as text rather than through buildPresetVar() so the test files
    // are exactly what a hand-written or foreign-built preset looks like -
    // including the key order, which a var-built document would normalise.
    juce::String makePresetFile (const juce::String& name,
                                  const juce::String& parametersJson,
                                  const juce::String& irJson,
                                  const juce::String& formatTag = "basilica-preset-1")
    {
        juce::String text;
        text << "{\n";
        text << "  \"format\": \"" << formatTag << "\",\n";
        text << "  \"plugin\": \"com.yvesvogl.nave\",\n";
        text << "  \"pluginVersion\": \"0.4.0\",\n";
        text << "  \"stateVersion\": 2,\n";
        text << "  \"name\": \"" << name << "\",\n";
        text << "  \"category\": \"Guitar\",\n";

        if (irJson.isNotEmpty())
            text << "  \"ir\": " << irJson << ",\n";

        text << "  \"parameters\": " << parametersJson << "\n";
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

    // A parameter block with values distinct from every layout default, so
    // "the parameters survived" is a real claim rather than a coincidence.
    const char* distinctParametersJson =
        R"({ "loCut": 90.0, "hiCut": 6200.0, "mix": 72.0, "level": -1.5,
             "irBlend": 40.0, "micDistance": 30.0, "blendMode": 1.0,
             "alignMode": 1.0, "irBTrim": -2.0, "irBPolarity": 0.0,
             "irBDelay": 0.25, "irGainMode": 1.0, "irAMinPhase": 0.0,
             "irBMinPhase": 0.0, "distanceAir": 1.0, "loCutSlope": 1.0,
             "hiCutSlope": 0.0 })";

    void checkDistinctParameters (NaveAudioProcessor& processor)
    {
        const auto value = [&processor] (const char* id)
        {
            auto* parameter = processor.apvts.getParameter (id);
            REQUIRE (parameter != nullptr);
            return parameter->convertFrom0to1 (parameter->getValue());
        };

        CHECK (value (ParamIDs::loCut) == Catch::Approx (90.0f).margin (0.5));
        CHECK (value (ParamIDs::hiCut) == Catch::Approx (6200.0f).margin (5.0));
        CHECK (value (ParamIDs::mix) == Catch::Approx (72.0f).margin (0.05));
        CHECK (value (ParamIDs::level) == Catch::Approx (-1.5f).margin (0.02));
        CHECK (value (ParamIDs::irBlend) == Catch::Approx (40.0f).margin (0.05));
        CHECK (value (ParamIDs::micDistance) == Catch::Approx (30.0f).margin (0.05));
        CHECK (value (ParamIDs::blendMode) == Catch::Approx (1.0f).margin (0.01));
        CHECK (value (ParamIDs::irBTrim) == Catch::Approx (-2.0f).margin (0.02));
        CHECK (value (ParamIDs::irBDelay) == Catch::Approx (0.25f).margin (0.01));
        CHECK (value (ParamIDs::irGainMode) == Catch::Approx (1.0f).margin (0.01));
        CHECK (value (ParamIDs::distanceAir) == Catch::Approx (1.0f).margin (0.01));
        CHECK (value (ParamIDs::loCutSlope) == Catch::Approx (1.0f).margin (0.01));
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
}

//==============================================================================
// D1 - the identifier tracks bytes.

TEST_CASE ("IR reference: the content hash is stable and depends on bytes, not identity",
           "[presets][ir][hash]")
{
    ScopedTestDirectory scratch ("hash");

    const auto original = writeIr (scratch.dir, "cab.wav", 0);

    const auto first = basilica::presets::contentHashOfFile (original);
    const auto second = basilica::presets::contentHashOfFile (original);

    REQUIRE (first.isNotEmpty());
    CHECK (first.length() == 64);
    CHECK (first.containsOnly ("0123456789abcdef"));

    // Hashing the same file twice must not depend on anything but its bytes.
    CHECK (first == second);

    // Same bytes under a different name in a different folder: the same
    // digest. This is what makes a reference survive a user reorganising
    // their library, and it is why resolution can be by hash alone.
    const auto renamedFolder = scratch.dir.getChildFile ("elsewhere");
    REQUIRE (renamedFolder.createDirectory());
    const auto copy = renamedFolder.getChildFile ("something-else-entirely.wav");
    REQUIRE (original.copyFileTo (copy));
    CHECK (basilica::presets::contentHashOfFile (copy) == first);

    // Different bytes: a different digest. A retune of a cabinet model is
    // exactly this case, and it is meant to MISS rather than resolve.
    const auto other = writeIr (scratch.dir, "other.wav", 3);
    CHECK (basilica::presets::contentHashOfFile (other) != first);

    // A file that is not there is not an error, it is simply no digest.
    CHECK (basilica::presets::contentHashOfFile (scratch.dir.getChildFile ("absent.wav")).isEmpty());
}

TEST_CASE ("IR reference: the bundled library's digests match its committed manifest",
           "[presets][ir][hash][content]")
{
    // The regeneration-stability claim, checked against the record the
    // generator itself produced: resources/irs/manifest.json's per-file
    // sha256 entries are written by tools/ir-synth/cabsynth.py, and CI
    // re-derives them from freshly generated audio
    // (tools/ir-synth/verify_irs.py, the "Verify bundled impulse responses"
    // step in .github/workflows/ci.yml). If a regeneration ever changed the
    // bytes, that step reddens; this test proves the digest THIS code
    // computes is the same one the manifest records, so a factory preset's
    // reference and the generator's provenance record cannot drift apart.
    const juce::File assetDir (juce::String (NAVE_IR_ASSET_DIR));
    REQUIRE (assetDir.isDirectory());

    const auto manifestFile = assetDir.getChildFile ("manifest.json");
    REQUIRE (manifestFile.existsAsFile());

    const auto manifest = juce::JSON::parse (manifestFile.loadFileAsString());
    const auto* irs = manifest.getProperty ("irs", {}).getArray();
    REQUIRE (irs != nullptr);
    REQUIRE (irs->size() == 9);

    for (const auto& entry : *irs)
    {
        const auto fileName = entry.getProperty ("file", {}).toString();
        const auto expected = entry.getProperty ("sha256", {}).toString();

        INFO ("bundled IR: " << fileName.toStdString());

        const auto file = assetDir.getChildFile (fileName);
        REQUIRE (file.existsAsFile());
        CHECK (basilica::presets::contentHashOfFile (file) == expected);
    }
}

TEST_CASE ("IR reference: a malformed or absent hash is no reference at all, not a broken one",
           "[presets][ir]")
{
    const auto parse = [] (const juce::String& irJson)
    {
        return basilica::presets::readIrReferences (
            juce::JSON::parse (makePresetFile ("X", "{}", irJson)));
    };

    CHECK_FALSE (basilica::presets::readIrReferences (
        juce::JSON::parse (makePresetFile ("X", "{}", {}))).isPresent());

    CHECK_FALSE (parse (R"({ "a": { "sha256": "not-a-hash", "name": "X" } })").isPresent());
    CHECK_FALSE (parse (R"({ "a": { "sha256": "", "name": "X" } })").isPresent());
    CHECK_FALSE (parse (R"({ "a": { "name": "X" } })").isPresent());

    // 64 characters, but not hex.
    CHECK_FALSE (parse (juce::String (R"({ "a": { "sha256": ")")
                         + juce::String::repeatedString ("z", 64) + R"(" } })").isPresent());

    const auto valid = parse (irJsonFor (juce::String::repeatedString ("ab", 32), "A Cab"));
    CHECK (valid.slotA.isPresent());
    CHECK (valid.slotA.displayName == "A Cab");
    CHECK_FALSE (valid.slotB.isPresent());
}

TEST_CASE ("IR reference: the content index finds a file by its bytes and never by its name",
           "[presets][ir]")
{
    ScopedTestDirectory scratch ("index");

    const auto wanted = writeIr (scratch.dir, "wanted.wav", 1);
    const auto decoy = writeIr (scratch.dir, "decoy.wav", 2);

    const auto wantedHash = basilica::presets::contentHashOfFile (wanted);
    const auto decoyHash = basilica::presets::contentHashOfFile (decoy);
    REQUIRE (wantedHash != decoyHash);

    basilica::ir::IrContentIndex index;
    index.setSearchRoots ({ scratch.dir });

    CHECK (index.findByContentHash (wantedHash) == wanted);
    CHECK (index.findByContentHash (decoyHash) == decoy);

    // Renaming the file the reference was made against must NOT stop it
    // resolving, and must not make the other file resolve in its place.
    const auto renamed = scratch.dir.getChildFile ("renamed.wav");
    REQUIRE (wanted.moveFileTo (renamed));
    index.clearCache();
    CHECK (index.findByContentHash (wantedHash) == renamed);

    // The bytes are gone: a miss, not a fallback to something similar.
    REQUIRE (renamed.deleteFile());
    index.clearCache();
    CHECK (index.findByContentHash (wantedHash) == juce::File());
}

//==============================================================================
// D3 (forward) - a v1 preset loads in this build.

TEST_CASE ("Preset format: a v1 preset (no IR reference) loads in this build with every parameter intact",
           "[presets][ir][compat]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("v1-presets");
    ScopedTestDirectory irDir ("v1-irs");

    const auto loadedBefore = writeIr (irDir.dir, "already-loaded.wav", 0);
    REQUIRE (processor.loadImpulseResponseFromFile (loadedBefore));

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    const auto file = presetDir.dir.getChildFile (juce::String ("Vintage v1") + PresetManager::presetFileExtension);
    REQUIRE (file.replaceWithText (makePresetFile ("Vintage v1", distinctParametersJson, {})));

    REQUIRE (manager.loadPreset ("Vintage v1"));

    checkDistinctParameters (processor);

    // A preset that references nothing must leave the IR slots completely
    // alone - that is what "the reference is optional" means in practice.
    CHECK (processor.getCurrentIrFilePath() == loadedBefore.getFullPathName());
    CHECK (processor.getPresetIrNotice().isEmpty());
}

//==============================================================================
// D3 (backward) - a v2 preset opens in a v1 build.

TEST_CASE ("Preset format: the v1 loader accepts a v2 preset and applies every parameter it understands",
           "[presets][ir][compat]")
{
    // The direction that gets forgotten. See tests/V1PresetLoaderReplica.h for
    // where this parse path comes from and why a replica is the only way to
    // exercise it in-process.
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    const auto v2Text = makePresetFile (
        "Made by a newer build",
        distinctParametersJson,
        irJsonFor (juce::String::repeatedString ("ab", 32), "Modelled 4x12 Ceramic Cone",
                   juce::String::repeatedString ("cd", 32), "Modelled 4x12 Room 1m"));

    const auto result = v1replica::loadPresetJson (processor.apvts, v2Text, "com.yvesvogl.nave");

    INFO ("v1 loader said: " << result.errorMessage.toStdString());
    REQUIRE (result.succeeded);

    // Not merely "it parsed": the parameters have to have been applied. A v1
    // build must present this preset as a working preset, not an empty one.
    checkDistinctParameters (processor);
}

TEST_CASE ("Preset format: the format tag is what a v1 build validates on, and it must not change",
           "[presets][ir][compat]")
{
    // The invariant the whole backward-compatibility story rests on, pinned
    // directly rather than inferred. v1's parseAndValidate() checks exactly
    // two fields and rejects on the format tag - so an added top-level key is
    // compatible and a bumped tag is not. If a future change bumps
    // presetFormatTag, this fails, and every preset written afterwards would
    // have been refused outright by every build already in the field.
    CHECK (juce::String (PresetManager::presetFormatTag) == "basilica-preset-1");
    CHECK (juce::String (PresetManager::presetFormatTag) == juce::String (v1replica::formatTag));

    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    const auto bumped = makePresetFile ("Hypothetical v2 tag", distinctParametersJson, {}, "basilica-preset-2");
    const auto rejected = v1replica::loadPresetJson (processor.apvts, bumped, "com.yvesvogl.nave");

    CHECK_FALSE (rejected.succeeded);
    CHECK (rejected.errorMessage.isNotEmpty());
}

//==============================================================================
// D1/D2 - resolving, and not resolving.

TEST_CASE ("Preset IR reference: a resolvable reference selects that IR, and the audio reaches the engine",
           "[presets][ir][processor]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("resolve-presets");
    ScopedTestDirectory irDir ("resolve-irs");

    const auto referenced = writeIr (irDir.dir, "referenced.wav", 1);
    const auto somethingElse = writeIr (irDir.dir, "something-else.wav", 2);

    useIrLibraryFolder (processor, irDir.dir);

    // Start from a DIFFERENT IR, so "the preset selected it" cannot be
    // satisfied by the slot happening to hold it already.
    REQUIRE (processor.loadImpulseResponseFromFile (somethingElse));
    REQUIRE (processor.getCurrentIrFilePath() == somethingElse.getFullPathName());

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    const auto referencedHash = basilica::presets::contentHashOfFile (referenced);
    const auto file = presetDir.dir.getChildFile (juce::String ("Referenced") + PresetManager::presetFileExtension);
    REQUIRE (file.replaceWithText (makePresetFile ("Referenced", distinctParametersJson,
                                                    irJsonFor (referencedHash, "Referenced Cab"))));

    REQUIRE (manager.loadPreset ("Referenced"));

    checkDistinctParameters (processor);
    CHECK (processor.getPresetIrNotice().isEmpty());
    CHECK (processor.getCurrentIrFilePath() == referenced.getFullPathName());

    // The claim that matters: the convolution engine is holding the
    // REFERENCED audio. A path property alone would not prove that.
    CHECK (buffersIdentical (processor.getLoadedImpulseResponse (0), readWav (referenced)));
    CHECK_FALSE (buffersIdentical (processor.getLoadedImpulseResponse (0), readWav (somethingElse)));
    CHECK (processor.getLoadedImpulseResponseSampleRate (0) == Catch::Approx (irSampleRate));
}

TEST_CASE ("Preset IR reference: both slots resolve independently",
           "[presets][ir][processor]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("two-slot-presets");
    ScopedTestDirectory irDir ("two-slot-irs");

    const auto forA = writeIr (irDir.dir, "for-a.wav", 1);
    const auto forB = writeIr (irDir.dir, "for-b.wav", 4);

    useIrLibraryFolder (processor, irDir.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    const auto file = presetDir.dir.getChildFile (juce::String ("Two Slots") + PresetManager::presetFileExtension);
    REQUIRE (file.replaceWithText (makePresetFile (
        "Two Slots", distinctParametersJson,
        irJsonFor (basilica::presets::contentHashOfFile (forA), "Cab A",
                    basilica::presets::contentHashOfFile (forB), "Cab B"))));

    REQUIRE (manager.loadPreset ("Two Slots"));

    CHECK (processor.getPresetIrNotice().isEmpty());
    CHECK (processor.getCurrentIrFilePath() == forA.getFullPathName());
    CHECK (processor.getCurrentIrFilePathB() == forB.getFullPathName());
    CHECK (buffersIdentical (processor.getLoadedImpulseResponse (0), readWav (forA)));
}

TEST_CASE ("Preset IR reference: a missing IR loads the preset, keeps the current IR, and substitutes nothing",
           "[presets][ir][processor]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("missing-presets");
    ScopedTestDirectory irDir ("missing-irs");
    ScopedTestDirectory offLibrary ("missing-outside");

    // In the library: the IR the user currently has up, plus a decoy that a
    // name-based or "nearest match" resolution would be tempted to reach for.
    const auto currentlyLoaded = writeIr (irDir.dir, "modelled_4x10_horn.wav", 1);
    const auto decoy = writeIr (irDir.dir, "decoy.wav", 2);

    // NOT in the library, and never will be: the referenced IR. Written
    // outside the search roots purely to obtain a real, well-formed digest.
    const auto absent = writeIr (offLibrary.dir, "absent.wav", 5);
    const auto absentHash = basilica::presets::contentHashOfFile (absent);

    useIrLibraryFolder (processor, irDir.dir);
    REQUIRE (processor.loadImpulseResponseFromFile (currentlyLoaded));

    juce::AudioBuffer<float> irBefore;
    irBefore.makeCopyOf (processor.getLoadedImpulseResponse (0));

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    const auto file = presetDir.dir.getChildFile (juce::String ("Missing Cab") + PresetManager::presetFileExtension);
    REQUIRE (file.replaceWithText (makePresetFile ("Missing Cab", distinctParametersJson,
                                                    irJsonFor (absentHash, "Modelled 4x10 Horn"))));

    // NEVER refuses to open.
    REQUIRE (manager.loadPreset ("Missing Cab"));

    // The parameters loaded in full.
    checkDistinctParameters (processor);

    // The IR slot is untouched - the same path AND the same samples. The
    // second half is the one that matters: it is what rules out a silent
    // substitution that happened to leave the path property behind.
    CHECK (processor.getCurrentIrFilePath() == currentlyLoaded.getFullPathName());
    CHECK (buffersIdentical (processor.getLoadedImpulseResponse (0), irBefore));
    CHECK (buffersIdentical (processor.getLoadedImpulseResponse (0), readWav (currentlyLoaded)));

    // ...and specifically NOT the decoy sitting right next to it in the same
    // folder, which is what a name- or proximity-based fallback would have
    // reached for.
    CHECK_FALSE (buffersIdentical (processor.getLoadedImpulseResponse (0), readWav (decoy)));

    // Degrades LOUDLY: a notice that names what was expected.
    const auto notice = processor.getPresetIrNotice();
    INFO ("notice: " << notice.toStdString());
    CHECK (notice.isNotEmpty());
    CHECK (notice.contains ("Modelled 4x10 Horn"));
    CHECK (notice.contains ("IR A"));

    // ...and the notice belongs to the preset that raised it: loading one
    // that resolves cleanly must clear it rather than leave it standing.
    const auto cleanFile = presetDir.dir.getChildFile (juce::String ("No Reference") + PresetManager::presetFileExtension);
    REQUIRE (cleanFile.replaceWithText (makePresetFile ("No Reference", distinctParametersJson, {})));
    REQUIRE (manager.loadPreset ("No Reference"));
    CHECK (processor.getPresetIrNotice().isEmpty());
}

TEST_CASE ("Preset IR reference: the notice reaches a listener, and names both slots when both miss",
           "[presets][ir][processor]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("notice-presets");
    ScopedTestDirectory irDir ("notice-irs");
    ScopedTestDirectory offLibrary ("notice-outside");

    useIrLibraryFolder (processor, irDir.dir);

    const auto absentA = writeIr (offLibrary.dir, "a.wav", 6);
    const auto absentB = writeIr (offLibrary.dir, "b.wav", 7);

    juce::StringArray delivered;
    processor.onPresetIrNotice = [&delivered] (const juce::String& message) { delivered.add (message); };

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    const auto file = presetDir.dir.getChildFile (juce::String ("Both Missing") + PresetManager::presetFileExtension);
    REQUIRE (file.replaceWithText (makePresetFile (
        "Both Missing", distinctParametersJson,
        irJsonFor (basilica::presets::contentHashOfFile (absentA), "Cab One",
                    basilica::presets::contentHashOfFile (absentB), "Cab Two"))));

    REQUIRE (manager.loadPreset ("Both Missing"));

    REQUIRE (delivered.size() == 1);
    CHECK (delivered[0] == processor.getPresetIrNotice());
    CHECK (delivered[0].contains ("Cab One"));
    CHECK (delivered[0].contains ("Cab Two"));
    CHECK (delivered[0].contains ("IR A"));
    CHECK (delivered[0].contains ("IR B"));

    processor.onPresetIrNotice = nullptr;
}

//==============================================================================
// Saving.

TEST_CASE ("Preset IR reference: saving with no IR loaded writes no reference at all",
           "[presets][ir]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("optional-presets");

    processor.loadDefaultImpulseResponse();
    processor.loadDefaultImpulseResponseB();

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    REQUIRE (manager.saveUserPreset ("No IR", "Guitar"));

    const auto written = presetDir.dir.getChildFile (juce::String ("No IR") + PresetManager::presetFileExtension)
                             .loadFileAsString();

    // The reference is OPTIONAL, and a preset that carries none has to be
    // exactly the document the pre-#42 saver produced - no empty "ir": {},
    // no null slots.
    CHECK_FALSE (written.contains ("\"ir\""));
    CHECK (written.contains ("basilica-preset-1"));
    CHECK_FALSE (basilica::presets::readIrReferences (juce::JSON::parse (written)).isPresent());
}

TEST_CASE ("Preset IR reference: saving with IRs loaded records their digests, and the file round-trips byte for byte",
           "[presets][ir]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("roundtrip-presets");
    ScopedTestDirectory irDir ("roundtrip-irs");

    const auto irA = writeIr (irDir.dir, "modelled_4x12_ceramic_cone.wav", 1);
    const auto irB = writeIr (irDir.dir, "modelled_4x12_ceramic_room.wav", 4);

    useIrLibraryFolder (processor, irDir.dir);
    REQUIRE (processor.loadImpulseResponseFromFile (irA));
    REQUIRE (processor.loadImpulseResponseFromFileB (irB));

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    REQUIRE (manager.saveUserPreset ("Round Trip", "Guitar"));

    const auto presetFile = presetDir.dir.getChildFile (juce::String ("Round Trip") + PresetManager::presetFileExtension);
    const auto firstSave = presetFile.loadFileAsString();

    const auto references = basilica::presets::readIrReferences (juce::JSON::parse (firstSave));
    REQUIRE (references.slotA.isPresent());
    REQUIRE (references.slotB.isPresent());
    CHECK (references.slotA.contentHash == basilica::presets::contentHashOfFile (irA));
    CHECK (references.slotB.contentHash == basilica::presets::contentHashOfFile (irB));

    // The display name is derived from the file, for the message only.
    CHECK (references.slotA.displayName == "Modelled 4x12 Ceramic Cone");

    // Perturb everything the preset is supposed to restore, then reload it.
    processor.loadDefaultImpulseResponse();
    processor.loadDefaultImpulseResponseB();
    auto* hiCut = processor.apvts.getParameter (ParamIDs::hiCut);
    REQUIRE (hiCut != nullptr);
    hiCut->setValueNotifyingHost (hiCut->convertTo0to1 (12345.0f));

    REQUIRE (manager.loadPreset ("Round Trip"));
    CHECK (processor.getCurrentIrFilePath() == irA.getFullPathName());
    CHECK (processor.getCurrentIrFilePathB() == irB.getFullPathName());

    REQUIRE (manager.saveCurrentUserPreset());
    const auto secondSave = presetFile.loadFileAsString();

    CHECK (secondSave == firstSave);
}

TEST_CASE ("Preset IR reference: renaming a user preset carries its reference across unchanged",
           "[presets][ir]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("rename-presets");
    ScopedTestDirectory irDir ("rename-irs");

    const auto irA = writeIr (irDir.dir, "renamed-cab.wav", 1);
    useIrLibraryFolder (processor, irDir.dir);
    REQUIRE (processor.loadImpulseResponseFromFile (irA));

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    REQUIRE (manager.saveUserPreset ("Before", "Guitar"));
    REQUIRE (manager.renameUserPreset ("Before", "After"));

    const auto renamed = presetDir.dir.getChildFile (juce::String ("After") + PresetManager::presetFileExtension);
    REQUIRE (renamed.existsAsFile());

    const auto references = basilica::presets::readIrReferences (juce::JSON::parse (renamed.loadFileAsString()));
    REQUIRE (references.slotA.isPresent());
    CHECK (references.slotA.contentHash == basilica::presets::contentHashOfFile (irA));

    // A rename relabels; it must not re-record the reference from whatever
    // happens to be loaded at the time.
    processor.loadDefaultImpulseResponse();
    REQUIRE (manager.loadPreset ("After"));
    CHECK (processor.getCurrentIrFilePath() == irA.getFullPathName());
}

//==============================================================================
// Factory presets.

TEST_CASE ("Factory presets: every reference resolves against the bundled library as shipped",
           "[presets][ir][factory][content]")
{
    // The only way a factory reference can be wrong without anything failing
    // is a digest that names no file Nave actually ships - so this installs
    // the EMBEDDED library (the copy in the binary, not the source tree),
    // points a processor at it, and loads every factory preset. A typo in any
    // reference shows up as a notice.
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("factory-presets");
    ScopedTestDirectory libraryDir ("factory-library");

    const auto install = basilica::ir::FactoryIrLibrary::installInto (libraryDir.dir, nave::factoryIrAssets());
    INFO ("install: " << install.summary().toStdString());
    REQUIRE (install.succeeded());

    useIrLibraryFolder (processor, libraryDir.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    const auto all = manager.getAllPresets();
    REQUIRE (all.size() == 10);

    int referencedPresets = 0;

    for (const auto& entry : all)
    {
        INFO ("factory preset: " << entry.name.toStdString());

        processor.loadDefaultImpulseResponse();
        processor.loadDefaultImpulseResponseB();

        REQUIRE (manager.loadPreset (entry.name));

        // Nothing missing: either the preset references nothing, or every
        // digest it names is a file the shipped library contains.
        CHECK (processor.getPresetIrNotice().isEmpty());

        if (processor.getCurrentIrFilePath().isNotEmpty())
        {
            ++referencedPresets;

            // Resolved from the installed library, not from anywhere else.
            CHECK (juce::File (processor.getCurrentIrFilePath()).isAChildOf (libraryDir.dir));
        }
    }

    // Not a vacuous pass: the three presets whose sound is a specific pair of
    // captures (Even Blend, Mic Morph, Touch of Room Mic - see docs/presets.md)
    // do carry references. The rest are tone-shaping recipes meant to apply to
    // whatever cabinet the player has, and deliberately reference nothing.
    CHECK (referencedPresets == 3);
}

TEST_CASE ("Factory presets: the referenced ones load both slots from the bundled library",
           "[presets][ir][factory][content]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("factory-pair-presets");
    ScopedTestDirectory libraryDir ("factory-pair-library");

    REQUIRE (basilica::ir::FactoryIrLibrary::installInto (libraryDir.dir, nave::factoryIrAssets()).succeeded());
    useIrLibraryFolder (processor, libraryDir.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    struct Expectation
    {
        const char* presetName;
        const char* slotAFile;
        const char* slotBFile;
    };

    // Each pairing is the one that preset's own entry in docs/presets.md
    // describes: "close+room" for the two blend-ratio presets, and two
    // positions on the SAME cabinet for Morph, which is the only material
    // Morph is meaningful on.
    const Expectation expectations[] = {
        { "Even Blend",        "modelled_4x12_ceramic_cone.wav", "modelled_4x12_ceramic_room.wav" },
        { "Touch of Room Mic", "modelled_4x12_ceramic_cone.wav", "modelled_4x12_ceramic_room.wav" },
        { "Mic Morph",         "modelled_4x12_ceramic_cone.wav", "modelled_4x12_ceramic_edge.wav" },
    };

    for (const auto& expectation : expectations)
    {
        INFO ("factory preset: " << expectation.presetName);

        processor.loadDefaultImpulseResponse();
        processor.loadDefaultImpulseResponseB();

        REQUIRE (manager.loadPreset (expectation.presetName));

        CHECK (processor.getPresetIrNotice().isEmpty());
        CHECK (juce::File (processor.getCurrentIrFilePath()).getFileName() == juce::String (expectation.slotAFile));
        CHECK (juce::File (processor.getCurrentIrFilePathB()).getFileName() == juce::String (expectation.slotBFile));

        // ...and the audio is genuinely in the convolver, not just named.
        CHECK (buffersIdentical (processor.getLoadedImpulseResponse (0),
                                  readWav (libraryDir.dir.getChildFile (expectation.slotAFile))));
    }
}

TEST_CASE ("Factory presets: with nothing on disk, they resolve from the embedded bundle",
           "[presets][ir][factory][content]")
{
    // SUPERSEDED BY ISSUE #45. Until then this case asserted the opposite -
    // that a first-run user meets a notice on a FACTORY preset naming content
    // that is already inside the binary they just installed. #45 decided the
    // embedded bytes are a resolution source, so the preset now loads its
    // cabinets with nothing on disk and nothing pressed.
    //
    // What did NOT change is decision D2: nothing is substituted, and a
    // reference nobody holds still degrades to an untouched slot plus a
    // notice - see tests/BundledIrResolutionTests.cpp, which owns the new
    // behaviour in full.
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("uninstalled-presets");
    ScopedTestDirectory emptyLibrary ("uninstalled-library");
    ScopedTestDirectory bundledCache ("uninstalled-cache");

    useIrLibraryFolder (processor, emptyLibrary.dir);

    // Never the real user location, which a test must not write into.
    processor.setBundledIrCacheDirectoryForTests (bundledCache.dir);

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    REQUIRE (manager.loadPreset ("Mic Morph"));

    // Parameters applied, per docs/presets.md's description of Mic Morph.
    auto* blendMode = processor.apvts.getParameter (ParamIDs::blendMode);
    REQUIRE (blendMode != nullptr);
    CHECK (blendMode->convertFrom0to1 (blendMode->getValue()) == Catch::Approx (1.0f).margin (0.01));

    auto* irBlend = processor.apvts.getParameter (ParamIDs::irBlend);
    REQUIRE (irBlend != nullptr);
    CHECK (irBlend->convertFrom0to1 (irBlend->getValue()) == Catch::Approx (35.0f).margin (0.05));

    // Both cabinets are up, out of the box, and there is nothing to say.
    const juce::File slotA (processor.getCurrentIrFilePath());
    const juce::File slotB (processor.getCurrentIrFilePathB());

    REQUIRE (slotA.existsAsFile());
    REQUIRE (slotB.existsAsFile());
    CHECK (slotA.isAChildOf (bundledCache.dir));
    CHECK (slotB.isAChildOf (bundledCache.dir));
    CHECK (slotA.getFileName() == "modelled_4x12_ceramic_cone.wav");
    CHECK (slotB.getFileName() == "modelled_4x12_ceramic_edge.wav");

    const auto notice = processor.getPresetIrNotice();
    INFO ("notice: " << notice.toStdString());
    CHECK (notice.isEmpty());
}

//==============================================================================
// The editor half of the notice.

TEST_CASE ("Preset IR reference: the editor shows the notice strip only while there is something to say",
           "[presets][ir][gui]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory presetDir ("gui-presets");
    ScopedTestDirectory irDir ("gui-irs");
    ScopedTestDirectory offLibrary ("gui-outside");

    const auto present = writeIr (irDir.dir, "present.wav", 1);
    const auto absent = writeIr (offLibrary.dir, "absent.wav", 5);

    useIrLibraryFolder (processor, irDir.dir);
    REQUIRE (processor.loadImpulseResponseFromFile (present));

    PresetManager manager (processor.apvts, makeIsolatedConfig (presetDir.dir), factoryPresetAssets());
    processor.installPresetIrCallbacks (manager);

    NaveAudioProcessorEditor editor (processor);

    auto* strip = editor.findChildWithID ("presetIrNotice");
    REQUIRE (strip != nullptr);

    // Nothing has gone wrong yet, so the strip takes up none of the faceplate.
    CHECK_FALSE (strip->isVisible());

    // It is not an interactive control, so it must stay out of the keyboard
    // focus order and must not swallow clicks meant for the art beneath it.
    CHECK_FALSE (strip->getWantsKeyboardFocus());

    const auto missingFile = presetDir.dir.getChildFile (juce::String ("Gui Missing") + PresetManager::presetFileExtension);
    REQUIRE (missingFile.replaceWithText (makePresetFile (
        "Gui Missing", distinctParametersJson,
        irJsonFor (basilica::presets::contentHashOfFile (absent), "Gui Cab"))));

    REQUIRE (manager.loadPreset ("Gui Missing"));

    CHECK (strip->isVisible());
    CHECK (strip->getTitle().contains ("Gui Cab")); // readable by assistive technology, not just on screen

    // Resolving cleanly puts it away again.
    const auto resolvingFile = presetDir.dir.getChildFile (juce::String ("Gui Present") + PresetManager::presetFileExtension);
    REQUIRE (resolvingFile.replaceWithText (makePresetFile (
        "Gui Present", distinctParametersJson,
        irJsonFor (basilica::presets::contentHashOfFile (present), "Present Cab"))));

    REQUIRE (manager.loadPreset ("Gui Present"));
    CHECK_FALSE (strip->isVisible());
}
