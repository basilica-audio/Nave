#pragma once

#include <juce_core/juce_core.h>

#include <vector>

// Installing the bundled factory IR library (issue #33) into the folder the
// IR browser already scans.
//
// WHY THIS EXISTS. The browser (src/ir/IrLibrary.h) is a directory scanner:
// it lists whatever WAV/AIFF files live under the library root, and nothing
// else. So a library that ships only inside the release archive is a library
// the plugin cannot see - the user has to find the archive again, find the
// "Impulse Responses" folder in it, and copy it to exactly the right place
// before the browser has anything to list. Every one of those steps is a way
// to end up staring at "No impulse responses found" with the library sitting
// on disk two folders away. The audio therefore also ships embedded, and this
// is what unpacks it to `IrLibrary::defaultDirectory()` on request.
//
// ON REQUEST, not silently. IrLibrary::defaultDirectory() documents that it
// deliberately does not create anything, and that stays true: nothing here
// runs unless the user asks for it (the browser's "Install Library" button,
// see IrBrowserPanel::onInstallFactoryLibrary). A plugin that writes into a
// user's Music folder the first time it is instantiated is doing something
// the user did not ask for.
//
// THREADING. File I/O throughout - message thread only, never the audio
// thread. Nothing in processBlock() reaches this file.
//
// PORTABILITY. Like src/presets/PresetManager.h, this has zero BinaryData.h
// dependency: the owning plugin builds the asset list from its own
// BinaryData:: symbols and passes it in (see PluginEditor.cpp), so a sibling
// plugin can reuse this file unchanged with its own bundled content.
namespace basilica::ir
{
    // One embedded factory-library file: the bytes juce_add_binary_data
    // produced, plus the name to install them under. Both the audio and the
    // provenance files (licence, manifest) travel through this same struct -
    // #33's licensing bar is "licence file committed alongside the audio",
    // and an install that dropped the audio somewhere without it would put
    // the shipped library out of compliance with the reason it exists.
    struct FactoryIrAsset
    {
        const char* fileName = nullptr;
        const char* data = nullptr;
        int dataSize = 0;

        // The cabinet's STABLE IDENTITY, matching the `id` field
        // resources/irs/manifest.json records for the same file
        // ("guitar-412-cone"). Null for the provenance files, which are not
        // cabinets.
        //
        // WHAT IT IS FOR, AND WHAT IT IS EMPHATICALLY NOT FOR. This names the
        // MODEL, not the audio. It is the handle documentation, the manifest,
        // the release notes and a sibling plugin use to talk about the same
        // cabinet across releases, and it is the thing #33 requires to survive
        // a file being renamed.
        //
        // It is NEVER a resolution key. Presets resolve by a hash of the
        // file's BYTES (src/presets/IrReference.h), precisely because an
        // identity-based id would silently follow a retuned model - the preset
        // would keep loading, keep looking correct, and quietly recall a
        // different sound. The two identifiers answer deliberately different
        // questions: the id asks "which cabinet is this", the digest asks "is
        // this exactly the audio the preset was made with". See
        // docs/bundled-ir-library.md for the release policy that keeps both
        // answers meaningful (in short: rename freely, never retune in place).
        const char* stableId = nullptr;
    };

    namespace FactoryIrLibrary
    {
        // What `destinationFolder` currently holds for one asset.
        enum class AssetState
        {
            missing,  // no such file
            differs,  // a file of that name exists, with other contents
            matches   // byte-identical to the embedded copy
        };

        // Compares by size first and only then by bytes, so the common
        // "already installed" case costs one stat per file rather than a
        // full read.
        AssetState stateOf (const juce::File& destinationFolder, const FactoryIrAsset& asset);

        // True when every asset is present and byte-identical. This is the
        // question the browser asks before deciding whether offering an
        // install would be useful, so it must not be answered by a mere
        // existence check: a half-copied or truncated file is exactly the
        // case where an install is *most* worth offering.
        bool isInstalledIn (const juce::File& destinationFolder,
                            const std::vector<FactoryIrAsset>& assets);

        struct InstallResult
        {
            int written = 0;         // assets newly written (or repaired)
            int alreadyPresent = 0;  // assets that were already byte-identical
            juce::StringArray failures;

            bool succeeded() const noexcept { return failures.isEmpty(); }

            // One line, suitable for the browser's status row.
            juce::String summary() const;
        };

        // Creates `destinationFolder` if needed and writes every asset that
        // is not already byte-identical, via juce::File::replaceWithData()
        // (JUCE 8.0.14: writes to a temporary and only then replaces, so an
        // interrupted install cannot leave a half-written IR behind for the
        // browser to hand to the convolver).
        //
        // Idempotent by construction: running it twice writes nothing the
        // second time. That also makes it a repair - a truncated or edited
        // file is rewritten, an untouched one is left alone.
        InstallResult installInto (const juce::File& destinationFolder,
                                   const std::vector<FactoryIrAsset>& assets);
    }
}
