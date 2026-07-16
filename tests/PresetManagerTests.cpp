#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "presets/PresetManager.h"
#include "dsp/CabConvolutionEngine.h"

#include <BinaryData.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <vector>

// M2 preset system tests (.scaffold/specs/preset-system-m2.md's "Tests"
// section - each TEST_CASE below maps to one of that section's numbered
// items, called out in the test names/comments).
namespace
{
    using basilica::presets::FactoryPresetAsset;
    using basilica::presets::PresetManager;
    using basilica::presets::PresetManagerConfig;

    // Mirrors PluginProcessor.cpp's own makeFactoryPresetAssets() - kept as
    // an independent copy here rather than exported from PluginProcessor.cpp
    // so this test file can construct its own, fully isolated PresetManager
    // instances (see makeIsolatedConfig() below) without depending on
    // production wiring internals.
    std::vector<FactoryPresetAsset> makeTestFactoryPresetAssets()
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
        };
    }

    // A fresh, isolated scratch directory per test case, so this file never
    // reads or writes the real ~/Library/Audio/Presets/... (or Windows
    // equivalent) location on the machine running the tests - see
    // PresetManagerConfig::userPresetsDirectoryOverrideForTests. Deleted on
    // destruction.
    struct ScopedTestDirectory
    {
        ScopedTestDirectory()
            : dir (juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("NavePresetManagerTests")
                       .getChildFile (juce::String (juce::Time::getHighResolutionTicks())
                                       + "_" + juce::String (juce::Random::getSystemRandom().nextInt (1000000))))
        {
            dir.createDirectory();
        }

        ~ScopedTestDirectory()
        {
            dir.deleteRecursively();
        }

        JUCE_DECLARE_NON_COPYABLE (ScopedTestDirectory)

        juce::File dir;
    };

    PresetManagerConfig makeIsolatedConfig (const juce::File& userDir)
    {
        PresetManagerConfig config;
        config.pluginId = "com.yvesvogl.nave";
        config.pluginName = "Nave";
        config.manufacturerName = "Yves Vogl";
        config.pluginVersion = "0.2.0-test";
        config.userPresetsDirectoryOverrideForTests = userDir;
        return config;
    }

    void setParam (NaveAudioProcessor& processor, const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }

    float getParam (NaveAudioProcessor& processor, const char* id)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        return param->convertFrom0to1 (param->getValue());
    }
}

//==============================================================================
// 1. Save -> load round-trip restores every parameter exactly.
TEST_CASE ("PresetManager: save -> load round-trip restores every parameter exactly", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::loCut, 250.0f);
    setParam (processor, ParamIDs::hiCut, 3500.0f);
    setParam (processor, ParamIDs::mix, 42.0f);
    setParam (processor, ParamIDs::level, -6.5f);
    setParam (processor, ParamIDs::irBlend, 35.0f);
    setParam (processor, ParamIDs::micDistance, 60.0f);

    REQUIRE (manager.saveUserPreset ("Round Trip", "Init"));

    // Perturb every parameter away from the saved values before reloading,
    // so the assertions below can't pass by accident.
    setParam (processor, ParamIDs::loCut, 20.0f);
    setParam (processor, ParamIDs::hiCut, 20000.0f);
    setParam (processor, ParamIDs::mix, 100.0f);
    setParam (processor, ParamIDs::level, 0.0f);
    setParam (processor, ParamIDs::irBlend, 0.0f);
    setParam (processor, ParamIDs::micDistance, 0.0f);

    REQUIRE (manager.loadPreset ("Round Trip"));

    CHECK (getParam (processor, ParamIDs::loCut) == Catch::Approx (250.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::hiCut) == Catch::Approx (3500.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::mix) == Catch::Approx (42.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::level) == Catch::Approx (-6.5f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::irBlend) == Catch::Approx (35.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::micDistance) == Catch::Approx (60.0f).margin (1.0e-3));
}

//==============================================================================
// 2. Import ignores unknown IDs, keeps defaults for missing IDs.
TEST_CASE ("PresetManager: import ignores unknown parameter IDs and keeps defaults for missing ones", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    // Move mix and level away from their defaults so it's meaningful when
    // the import below leaves them untouched (they're absent from
    // "parameters").
    setParam (processor, ParamIDs::mix, 55.0f);
    setParam (processor, ParamIDs::level, 3.0f);

    // A fixture JSON generated inline (not committed under tests/fixtures/)
    // to avoid brittle relative-path resolution across CI runners with
    // different working directories (macOS vs Windows ctest invocations) -
    // this is the forward/backward-compat scenario the spec's "fixture
    // JSONs in tests/" line calls for: an unknown ID ("futureParameter",
    // simulating a newer plugin version's preset) and two known IDs
    // (loCut/hiCut), deliberately omitting mix/level/irBlend/micDistance.
    const juce::String fixtureJson = R"({
        "format": "basilica-preset-1",
        "plugin": "com.yvesvogl.nave",
        "pluginVersion": "9.9.9",
        "name": "Forward Compat Fixture",
        "category": "Init",
        "parameters": { "loCut": 120.0, "hiCut": 6000.0, "futureParameter": 42.0 }
    })";

    const auto fixtureFile = juce::File::createTempFile (".basilicapreset");
    REQUIRE (fixtureFile.replaceWithText (fixtureJson));

    juce::String errorMessage;
    REQUIRE (manager.importPresetFile (fixtureFile, errorMessage));
    CHECK (errorMessage.isEmpty());

    // Known IDs present in the fixture were applied...
    CHECK (getParam (processor, ParamIDs::loCut) == Catch::Approx (120.0f).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::hiCut) == Catch::Approx (6000.0f).margin (1.0e-3));

    // ...IDs absent from the fixture were reset to their ParameterLayout
    // defaults (loadPreset()/importPresetFile() always reset-then-apply -
    // see PresetManager.h), not left at the pre-import 55%/3 dB values.
    auto* mixParam = processor.apvts.getParameter (ParamIDs::mix);
    auto* levelParam = processor.apvts.getParameter (ParamIDs::level);
    CHECK (getParam (processor, ParamIDs::mix) == Catch::Approx (mixParam->convertFrom0to1 (mixParam->getDefaultValue())).margin (1.0e-3));
    CHECK (getParam (processor, ParamIDs::level) == Catch::Approx (levelParam->convertFrom0to1 (levelParam->getDefaultValue())).margin (1.0e-3));

    fixtureFile.deleteFile();
}

//==============================================================================
// 3. Import refuses wrong-plugin and wrong-format files.
TEST_CASE ("PresetManager: import refuses a preset belonging to a different plugin", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    const juce::String wrongPluginJson = R"({
        "format": "basilica-preset-1",
        "plugin": "com.yvesvogl.overture",
        "pluginVersion": "0.2.0",
        "name": "Not Nave's",
        "category": "Init",
        "parameters": { "loCut": 999.0 }
    })";

    const auto file = juce::File::createTempFile (".basilicapreset");
    REQUIRE (file.replaceWithText (wrongPluginJson));

    juce::String errorMessage;
    CHECK_FALSE (manager.importPresetFile (file, errorMessage));
    CHECK (errorMessage.isNotEmpty());

    // State must be left untouched - loCut must NOT have picked up 999 (out
    // of its own 20-800 Hz range too, which would be a separate bug on top).
    CHECK (getParam (processor, ParamIDs::loCut) != Catch::Approx (999.0f));

    file.deleteFile();
}

TEST_CASE ("PresetManager: import refuses a file with an incompatible format tag", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    const juce::String wrongFormatJson = R"({
        "format": "some-other-format-2",
        "plugin": "com.yvesvogl.nave",
        "pluginVersion": "0.2.0",
        "name": "Wrong Format",
        "category": "Init",
        "parameters": { "loCut": 999.0 }
    })";

    const auto file = juce::File::createTempFile (".basilicapreset");
    REQUIRE (file.replaceWithText (wrongFormatJson));

    juce::String errorMessage;
    CHECK_FALSE (manager.importPresetFile (file, errorMessage));
    CHECK (errorMessage.isNotEmpty());

    file.deleteFile();
}

//==============================================================================
// 4. Factory presets all parse and load.
TEST_CASE ("PresetManager: every factory preset parses and loads without error", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    const auto all = manager.getAllPresets();
    const auto factoryCount = std::count_if (all.begin(), all.end(), [] (auto& e) { return e.isFactory; });

    REQUIRE (factoryCount == 8); // design-brief.md's Factory Presets section

    for (auto& entry : all)
    {
        if (! entry.isFactory)
            continue;

        CAPTURE (entry.name);
        CHECK (manager.loadPreset (entry.name));
        CHECK (manager.isCurrentPresetFactory());
        CHECK (manager.getCurrentPresetName() == entry.name);
    }
}

TEST_CASE ("PresetManager: factory preset content is plausible (Default is Init category, all parameters in range)", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    const auto all = manager.getAllPresets();
    const auto defaultEntry = std::find_if (all.begin(), all.end(), [] (auto& e) { return e.name == "Default"; });

    REQUIRE (defaultEntry != all.end());
    CHECK (defaultEntry->category == "Init");
    CHECK (defaultEntry->isFactory);

    // Loading every factory preset must leave every parameter's live value
    // inside its own ParameterLayout range - APVTS's setValueNotifyingHost()
    // clamps out-of-range normalised input, so an out-of-range preset value
    // wouldn't crash, but it would silently mean the JSON doesn't say what
    // the plugin actually does - worth catching explicitly.
    for (auto& entry : all)
    {
        if (! entry.isFactory)
            continue;

        REQUIRE (manager.loadPreset (entry.name));

        CHECK (getParam (processor, ParamIDs::loCut) >= CabConvolutionEngine::loCutMinHz);
        CHECK (getParam (processor, ParamIDs::loCut) <= CabConvolutionEngine::loCutMaxHz);
        CHECK (getParam (processor, ParamIDs::hiCut) >= CabConvolutionEngine::hiCutMinHz);
        CHECK (getParam (processor, ParamIDs::hiCut) <= CabConvolutionEngine::hiCutMaxHz);
        CHECK (getParam (processor, ParamIDs::mix) >= 0.0f);
        CHECK (getParam (processor, ParamIDs::mix) <= 100.0f);
    }
}

//==============================================================================
// 5. Default resolution order (user Default > factory Default > plain defaults).
TEST_CASE ("PresetManager: applyStartupDefault() loads the factory Default when no user Default exists", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::loCut, 555.0f); // perturb first

    manager.applyStartupDefault();

    CHECK (manager.getCurrentPresetName() == "Default");
    CHECK (manager.isCurrentPresetFactory());
    CHECK (getParam (processor, ParamIDs::loCut) == Catch::Approx (CabConvolutionEngine::loCutMinHz).margin (1.0e-3));
}

TEST_CASE ("PresetManager: a user Default preset wins over the factory Default", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::loCut, 321.0f);
    REQUIRE (manager.setCurrentAsDefault()); // writes a user preset literally named "Default"

    setParam (processor, ParamIDs::loCut, 20.0f); // perturb away before the resolution check

    manager.applyStartupDefault();

    CHECK (manager.getCurrentPresetName() == "Default");
    CHECK_FALSE (manager.isCurrentPresetFactory()); // resolved to the *user* Default, not the factory one
    CHECK (getParam (processor, ParamIDs::loCut) == Catch::Approx (321.0f).margin (1.0e-3));
}

TEST_CASE ("PresetManager: resetDefault() removes the user Default so the factory Default resolves again", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::loCut, 321.0f);
    REQUIRE (manager.setCurrentAsDefault());
    REQUIRE (manager.resetDefault());

    manager.applyStartupDefault();

    CHECK (manager.isCurrentPresetFactory());
    CHECK (getParam (processor, ParamIDs::loCut) == Catch::Approx (CabConvolutionEngine::loCutMinHz).margin (1.0e-3));
}

//==============================================================================
// 6. Dirty flag: clean after load, dirty after any param change, clean after save.
TEST_CASE ("PresetManager: dirty flag lifecycle - clean after load, dirty after a change, clean after save", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    REQUIRE (manager.loadPreset ("Default"));
    CHECK_FALSE (manager.isDirty());

    setParam (processor, ParamIDs::loCut, 300.0f);
    CHECK (manager.isDirty());

    REQUIRE (manager.saveUserPreset ("Dirty Flag Preset", "Init"));
    CHECK_FALSE (manager.isDirty());
}

//==============================================================================
// 7. prev/next ordering and wrap-around.
TEST_CASE ("PresetManager: nextPreset()/previousPreset() traverse alphabetically and wrap around", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    const auto all = manager.getAllPresets();
    REQUIRE (all.size() >= 2);

    REQUIRE (manager.loadPreset (all.front().name));

    manager.nextPreset();
    CHECK (manager.getCurrentPresetName() == all[1].name);

    manager.previousPreset();
    CHECK (manager.getCurrentPresetName() == all.front().name);

    // Wrap backward from the first entry to the last.
    manager.previousPreset();
    CHECK (manager.getCurrentPresetName() == all.back().name);

    // Wrap forward from the last entry back to the first.
    manager.nextPreset();
    CHECK (manager.getCurrentPresetName() == all.front().name);
}

//==============================================================================
// Additional coverage beyond the spec's minimum list: save/rename/delete
// guards, single-file export round-trip, and bank import/export.

TEST_CASE ("PresetManager: saveUserPreset() refuses to shadow a factory preset name", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    CHECK_FALSE (manager.saveUserPreset ("Default", "Init")); // "Default" already exists as a factory preset
    CHECK_FALSE (manager.saveUserPreset ("Tame the Fizz", "Guitar"));
}

TEST_CASE ("PresetManager: renameUserPreset() moves a user preset to a new name and preserves its parameters", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::hiCut, 4321.0f);
    REQUIRE (manager.saveUserPreset ("Old Name", "Init"));

    REQUIRE (manager.renameUserPreset ("Old Name", "New Name"));

    setParam (processor, ParamIDs::hiCut, 20000.0f); // perturb before reloading

    CHECK_FALSE (manager.loadPreset ("Old Name")); // gone
    REQUIRE (manager.loadPreset ("New Name"));
    CHECK (getParam (processor, ParamIDs::hiCut) == Catch::Approx (4321.0f).margin (1.0e-3));
}

TEST_CASE ("PresetManager: deleteUserPreset() removes a user preset but never a factory preset", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    REQUIRE (manager.saveUserPreset ("Temporary", "Init"));
    REQUIRE (manager.deleteUserPreset ("Temporary"));
    CHECK_FALSE (manager.loadPreset ("Temporary"));

    // A factory preset name isn't a file on disk in the user directory, so
    // there's nothing to delete - deleteUserPreset() must return false, and
    // the factory preset must still load afterwards.
    CHECK_FALSE (manager.deleteUserPreset ("Default"));
    CHECK (manager.loadPreset ("Default"));
}

TEST_CASE ("PresetManager: exportPreset()/importPresetFile() single-file round-trip", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    setParam (processor, ParamIDs::micDistance, 77.0f);
    REQUIRE (manager.saveUserPreset ("Exportable", "Init"));

    const auto exportFile = juce::File::createTempFile (".basilicapreset");
    REQUIRE (manager.exportPreset ("Exportable", exportFile));
    REQUIRE (exportFile.existsAsFile());

    REQUIRE (manager.deleteUserPreset ("Exportable")); // remove the original before reimporting

    juce::String errorMessage;
    REQUIRE (manager.importPresetFile (exportFile, errorMessage));
    CHECK (getParam (processor, ParamIDs::micDistance) == Catch::Approx (77.0f).margin (1.0e-3));

    exportFile.deleteFile();
}

TEST_CASE ("PresetManager: exportBank()/importBank() round-trips every user preset through a zip", "[presets]")
{
    ScopedTestDirectory sourceScratch;
    ScopedTestDirectory destScratch;

    NaveAudioProcessor sourceProcessor;
    sourceProcessor.prepareToPlay (48000.0, 512);
    PresetManager sourceManager (sourceProcessor.apvts, makeIsolatedConfig (sourceScratch.dir), makeTestFactoryPresetAssets());

    setParam (sourceProcessor, ParamIDs::loCut, 111.0f);
    REQUIRE (sourceManager.saveUserPreset ("Bank Preset A", "Init"));

    setParam (sourceProcessor, ParamIDs::loCut, 222.0f);
    REQUIRE (sourceManager.saveUserPreset ("Bank Preset B", "Init"));

    const auto bankFile = juce::File::createTempFile (".zip");
    REQUIRE (sourceManager.exportBank (bankFile));
    REQUIRE (bankFile.existsAsFile());

    NaveAudioProcessor destProcessor;
    destProcessor.prepareToPlay (48000.0, 512);
    PresetManager destManager (destProcessor.apvts, makeIsolatedConfig (destScratch.dir), makeTestFactoryPresetAssets());

    const auto importedCount = destManager.importBank (bankFile);
    CHECK (importedCount == 2);

    REQUIRE (destManager.loadPreset ("Bank Preset A"));
    CHECK (getParam (destProcessor, ParamIDs::loCut) == Catch::Approx (111.0f).margin (1.0e-3));

    REQUIRE (destManager.loadPreset ("Bank Preset B"));
    CHECK (getParam (destProcessor, ParamIDs::loCut) == Catch::Approx (222.0f).margin (1.0e-3));

    bankFile.deleteFile();
}

//==============================================================================
// 8. PresetManager never allocates or locks on the audio thread.
//
// Verified primarily *by design*: nothing in NaveAudioProcessor::
// processBlock()/CabConvolutionEngine ever calls into PresetManager (see
// PluginProcessor.cpp - presetManager is only touched from the constructor
// and from PresetBar's message-thread-only UI callbacks), so there is no
// code path for this test to exercise in the first place. The one nuance is
// PresetManager::parameterChanged() (an AudioProcessorValueTreeState::
// Listener callback that JUCE does not document as guaranteed message-
// thread-only) - it is implemented as a single lock-free std::atomic<bool>
// store and nothing else (see PresetManager.h/.cpp), which this test
// exercises indirectly by driving parameter changes and processBlock() back
// to back and confirming nothing misbehaves.
TEST_CASE ("PresetManager: parameter-driven dirty tracking coexists safely with real-time audio processing", "[presets]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);

    ScopedTestDirectory scratch;
    PresetManager manager (processor.apvts, makeIsolatedConfig (scratch.dir), makeTestFactoryPresetAssets());

    REQUIRE (manager.loadPreset ("Default"));
    CHECK_FALSE (manager.isDirty());

    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        // Every parameterChanged() callback below happens interleaved with
        // real audio processing - if it ever became audio-thread-unsafe
        // (e.g. someone later added a lock or allocation to it), a helgrind/
        // TSan CI run would be the real detector; this test's job is just to
        // confirm normal operation isn't disrupted by the two coexisting.
        setParam (processor, ParamIDs::loCut, 20.0f + static_cast<float> (block) * 10.0f);
        CHECK_NOTHROW (processor.processBlock (buffer, midi));
    }

    CHECK (manager.isDirty());
}
