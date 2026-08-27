#include "PluginEditor.h"
#include "ir/FactoryIrLibrary.h"
#include "ir/IrContentIndex.h"
#include "presets/IrReference.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

// The bundled IR library as a CURATED SET (issue #33), rather than as nine
// signals - tests/FactoryIrLibraryTests.cpp already measures the audio, and
// tools/ir-synth/verify_irs.py measures the files. What is asserted here is
// everything about the set that is a DECISION and could therefore be changed
// by accident:
//
//   C1  MEMBERSHIP. Exactly nine cabinets, and exactly these nine ids. The
//       reasoning is docs/adr/0004-bundled-ir-library-curation.md; this is the
//       ratchet that makes it enforceable rather than merely written down.
//
//   C2  IDENTITY. Every embedded cabinet carries a stable id, the ids are
//       unique, and each matches the `id` resources/irs/manifest.json records
//       for the same file. The id survives a rename; that is what it is for.
//
//   C3  THE ID IS NOT A RESOLUTION KEY. Presets resolve by a hash of the
//       file's bytes (#42). An id is not a digest and must never behave like
//       one, because an identity-based lookup would silently follow a retuned
//       model - the failure the byte hash exists to prevent.
//
//   C4  PROVENANCE TRAVELS WITH THE AUDIO. #33's licensing bar is a licence
//       committed ALONGSIDE the audio, so an install that dropped the
//       provenance would put the shipped library out of compliance with the
//       reason it exists. Every cabinet must also be named in LICENSES.md - a
//       file nobody documented is a file nobody can vouch for.
//
//   C5  FOOTPRINT. The bundle costs what the documentation says it costs,
//       measured from the bytes actually compiled into this binary.
//
// Nothing here reads audio or renders anything; it is a set/manifest/documentation
// consistency pass, and it is not wall-clock sensitive.
namespace
{
    using basilica::ir::FactoryIrAsset;

    const std::vector<FactoryIrAsset>& assets() { return nave::factoryIrAssets(); }

    // NAVE_IR_ASSET_DIR is set by CMakeLists.txt to the in-repo asset
    // directory. The manifest and LICENSES.md are read from there rather than
    // from BinaryData so that a drift between the source tree and the embedded
    // copy shows up as a failure here (FactoryIrInstallTests.cpp separately
    // pins that the two are byte-identical).
    juce::File assetDir()
    {
        return juce::File (juce::String (NAVE_IR_ASSET_DIR));
    }

    bool isCabinet (const FactoryIrAsset& asset)
    {
        return asset.fileName != nullptr && juce::String (asset.fileName).endsWithIgnoreCase (".wav");
    }

    struct ManifestEntry
    {
        juce::String id;
        juce::String file;
        juce::String sha256;
        juce::String family;
    };

    std::vector<ManifestEntry> readManifest()
    {
        const auto file = assetDir().getChildFile ("manifest.json");
        REQUIRE (file.existsAsFile());

        const auto parsed = juce::JSON::parse (file.loadFileAsString());
        auto* root = parsed.getDynamicObject();
        REQUIRE (root != nullptr);

        const auto irs = root->getProperty ("irs");
        auto* list = irs.getArray();
        REQUIRE (list != nullptr);

        std::vector<ManifestEntry> entries;

        for (const auto& item : *list)
        {
            auto* object = item.getDynamicObject();
            REQUIRE (object != nullptr);

            entries.push_back ({ object->getProperty ("id").toString(),
                                 object->getProperty ("file").toString(),
                                 object->getProperty ("sha256").toString().toLowerCase(),
                                 object->getProperty ("family").toString() });
        }

        return entries;
    }
}

//==============================================================================
// C1 - membership.

TEST_CASE ("Bundled IR curation: the shipped set is exactly the nine decided cabinets",
           "[ir][curation][content]")
{
    // THE RATCHET. These ids are permanent: once an id has shipped, a preset
    // may refer to that cabinet by name in a release note or a manual forever
    // after. Removing or renaming one is a breaking change for documentation
    // and belongs in a major version, so it has to fail the build rather than
    // pass review unnoticed. Adding one is additive - append it here and to
    // resources/irs/LICENSES.md.
    //
    // The membership rationale (why nine, why these) is
    // docs/adr/0004-bundled-ir-library-curation.md.
    const std::set<juce::String> decidedIds {
        "guitar-412-cone",
        "guitar-412-edge",
        "guitar-412-room",
        "guitar-212-alnico",
        "guitar-112-combo",
        "bass-810-cone",
        "bass-810-edge",
        "bass-115-vintage",
        "bass-410-horn",
    };

    std::set<juce::String> shippedIds;

    for (const auto& asset : assets())
    {
        if (! isCabinet (asset))
            continue;

        REQUIRE (asset.stableId != nullptr);
        shippedIds.insert (asset.stableId);
    }

    CHECK (shippedIds.size() == 9);
    CHECK (shippedIds == decidedIds);

    // Both families are represented, which is the load-bearing half of "a
    // cabinet IR loader for guitar AND bass": a set that lost every bass
    // cabinet would still be nine files.
    const auto countWithPrefix = [&shippedIds] (const char* prefix)
    {
        return std::count_if (shippedIds.begin(), shippedIds.end(),
                              [prefix] (const juce::String& id) { return id.startsWith (prefix); });
    };

    CHECK (countWithPrefix ("guitar-") == 5);
    CHECK (countWithPrefix ("bass-") == 4);

    // The two mic-position PAIRS the DSP features are built around (IR Blend's
    // close+room, Morph's two positions on one cabinet - see
    // docs/presets.md). A set without them makes three factory presets silent
    // no-ops again, which is what #44 fixed.
    CHECK (shippedIds.count ("guitar-412-cone") == 1);
    CHECK (shippedIds.count ("guitar-412-edge") == 1);
    CHECK (shippedIds.count ("guitar-412-room") == 1);
    CHECK (shippedIds.count ("bass-810-cone") == 1);
    CHECK (shippedIds.count ("bass-810-edge") == 1);
}

//==============================================================================
// C2 - identity.

TEST_CASE ("Bundled IR curation: every cabinet's id is unique and matches the manifest",
           "[ir][curation][content]")
{
    const auto manifest = readManifest();
    REQUIRE (manifest.size() == 9);

    std::map<juce::String, juce::String> manifestIdForFile;
    std::set<juce::String> manifestIds;

    for (const auto& entry : manifest)
    {
        INFO ("manifest entry: " << entry.file.toStdString());
        CHECK (entry.id.isNotEmpty());
        CHECK (entry.file.isNotEmpty());
        CHECK (entry.sha256.length() == 64);
        CHECK ((entry.family == "guitar" || entry.family == "bass"));

        // One id per file, one file per id - in both directions, so a
        // copy-paste that gave two cabinets the same id cannot pass.
        CHECK (manifestIds.insert (entry.id).second);
        CHECK (manifestIdForFile.insert ({ entry.file, entry.id }).second);
    }

    std::set<juce::String> embeddedIds;
    int cabinets = 0;

    for (const auto& asset : assets())
    {
        if (! isCabinet (asset))
            continue;

        ++cabinets;

        const juce::String fileName (asset.fileName);
        INFO ("embedded asset: " << fileName.toStdString());

        REQUIRE (asset.stableId != nullptr);
        const juce::String id (asset.stableId);

        CHECK (id.isNotEmpty());
        CHECK (embeddedIds.insert (id).second);

        const auto found = manifestIdForFile.find (fileName);
        REQUIRE (found != manifestIdForFile.end());
        CHECK (id == found->second);
    }

    CHECK (cabinets == 9);
    CHECK (embeddedIds == manifestIds);

    // The provenance files are not cabinets and must not carry an id - an id
    // on LICENSES.md would be a claim that a text file is a model.
    for (const auto& asset : assets())
        if (! isCabinet (asset))
            CHECK (asset.stableId == nullptr);
}

TEST_CASE ("Bundled IR curation: the id survives a rename, which is what it is for",
           "[ir][curation]")
{
    // The property the id exists to have, stated as a test rather than as a
    // comment: the identity of a cabinet is not its file name. Renaming the
    // file changes what the browser lists and changes nothing else - not the
    // id, and not the digest a preset resolves by.
    const auto cone = std::find_if (assets().begin(), assets().end(),
                                     [] (const FactoryIrAsset& asset)
                                     {
                                         return asset.stableId != nullptr
                                             && juce::String (asset.stableId) == "guitar-412-cone";
                                     });

    REQUIRE (cone != assets().end());
    CHECK (juce::String (cone->fileName) == "modelled_4x12_ceramic_cone.wav");

    // Deliberately asserted the other way round too: the id is NOT derivable
    // from the file name. If it were, it would break on exactly the rename it
    // is supposed to survive.
    CHECK (juce::String (cone->stableId) != juce::String (cone->fileName).upToLastOccurrenceOf (".", false, false));
}

//==============================================================================
// C3 - the id is not a resolution key.

TEST_CASE ("Bundled IR curation: an id is not a digest and cannot resolve anything",
           "[ir][curation][presets]")
{
    // #42 chose a hash of the file's BYTES precisely so that a retuned model
    // misses loudly instead of being silently recalled under its old identity.
    // That guarantee only holds while nothing anywhere accepts an id as a
    // lookup key, so the resolution surface is asked directly.
    const auto irDir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                           .getChildFile ("NaveBundledIrCurationTests")
                           .getChildFile ("roots_" + juce::String (juce::Time::getHighResolutionTicks()));

    REQUIRE (irDir.createDirectory().wasOk());

    basilica::ir::IrContentIndex index;
    index.setSearchRoots ({ irDir });

    for (const auto& asset : assets())
    {
        if (! isCabinet (asset))
            continue;

        REQUIRE (asset.stableId != nullptr);

        const juce::String id (asset.stableId);

        // Not 64 hex characters, so it cannot even be mistaken for a digest.
        CHECK (id.length() != 64);
        CHECK (index.findByContentHash (id) == juce::File());
    }

    irDir.deleteRecursively();
}

//==============================================================================
// C4 - provenance travels with the audio.

TEST_CASE ("Bundled IR curation: every cabinet is documented, and the provenance ships with it",
           "[ir][curation][content]")
{
    // The licensing bar from #33 is a licence file committed ALONGSIDE the
    // audio. Three things have to hold for that to be true of a shipped
    // binary rather than of this repository: the provenance is embedded, it
    // is embedded with the audio, and it actually names every file.
    const auto embeds = [] (const char* fileName)
    {
        return std::any_of (assets().begin(), assets().end(),
                            [fileName] (const FactoryIrAsset& asset)
                            {
                                return asset.fileName != nullptr
                                    && juce::String (asset.fileName) == fileName
                                    && asset.data != nullptr
                                    && asset.dataSize > 0;
                            });
    };

    CHECK (embeds ("LICENSES.md"));
    CHECK (embeds ("CC0-1.0.txt"));
    CHECK (embeds ("manifest.json"));

    const auto licences = assetDir().getChildFile ("LICENSES.md");
    REQUIRE (licences.existsAsFile());

    const auto text = licences.loadFileAsString();

    // CC0 by name, so a future edit cannot quietly weaken the licence claim
    // the whole bundle rests on.
    CHECK (text.contains ("CC0 1.0 Universal"));

    for (const auto& asset : assets())
    {
        if (! isCabinet (asset))
            continue;

        const juce::String fileName (asset.fileName);
        INFO ("cabinet: " << fileName.toStdString());

        // Named, and named by its id as well - the id is the handle the
        // documentation has to use if it is to survive a rename.
        CHECK (text.contains (fileName));

        REQUIRE (asset.stableId != nullptr);
        CHECK (text.contains (juce::String (asset.stableId)));

        // And labelled as a model. #33 made unambiguous labelling a binding
        // condition on ever shipping synthetic IRs.
        CHECK (fileName.startsWith ("modelled_"));
    }
}

//==============================================================================
// C5 - footprint.

TEST_CASE ("Bundled IR curation: the bundle costs what the documentation says it costs",
           "[ir][curation][content]")
{
    // Measured from the bytes actually compiled into this binary, not from the
    // source tree, so the documented figure cannot drift away from what users
    // download.
    //
    // NOTE THAT EDITING resources/irs/LICENSES.md FAILS THIS TEST. That is
    // intended, not a nuisance: LICENSES.md ships inside the binary, so its
    // own size is part of the footprint, and the number this test pins is
    // printed in that same file's Footprint table. The two have to be updated
    // together or the documentation is wrong the moment it is edited. The
    // failure message prints the new figure.
    //
    // IT IS ALSO WHY .gitattributes MARKS THE EMBEDDED ASSETS `-text`. This
    // assertion first failed on Windows CI at 34,919 provenance bytes against
    // macOS's 34,137 - Git's default text=auto had rewritten the three
    // embedded provenance files to CRLF in the working tree, so the same
    // commit was shipping different bytes depending on who built it. The
    // .wav cabinets were never affected (they contain NUL bytes, so Git
    // treats them as binary), which is why nothing about the preset -> IR
    // content hashes ever depended on this. A platform-dependent figure here
    // is a real defect, not a tolerance to widen.

    constexpr int expectedAudioBytes = 98700;
    constexpr int expectedProvenanceBytes = 35140;
    constexpr int expectedTotalBytes = expectedAudioBytes + expectedProvenanceBytes; // 133,840 = 130.7 KiB

    int audioBytes = 0;
    int provenanceBytes = 0;

    for (const auto& asset : assets())
    {
        REQUIRE (asset.data != nullptr);
        REQUIRE (asset.dataSize > 0);

        (isCabinet (asset) ? audioBytes : provenanceBytes) += asset.dataSize;
    }

    CHECK (audioBytes == expectedAudioBytes);
    CHECK (provenanceBytes == expectedProvenanceBytes);
    CHECK (audioBytes + provenanceBytes == expectedTotalBytes);

    // A ceiling as well as an equality, so that a future addition has to be a
    // deliberate one: a quarter of a megabyte of embedded assets in a plugin
    // whose whole point is that the user brings their own IRs would be a
    // different product decision, not a routine commit.
    CHECK (audioBytes + provenanceBytes < 256 * 1024);
}
