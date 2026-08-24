#pragma once

#include <juce_core/juce_core.h>

// Preset -> impulse-response references (issue #42).
//
// WHY A REFERENCE EXISTS AT ALL. In a cabinet-IR plugin the cab dominates the
// perceived tone, so a preset that recalls every parameter EXCEPT the cabinet
// does not recall the sound it is named after - it recalls a filter setting
// applied to whatever the player happened to have loaded. The reference is
// optional and overridable: a preset carrying none behaves exactly as before
// this file existed, and a player who loads a preset and then swaps the cab is
// doing the intended thing.
//
// WHY THE IDENTIFIER IS A CONTENT HASH AND NOT A NAME OR AN ID. The bundled
// IRs come out of tools/ir-synth/cabsynth.py byte-identically on regeneration
// (verified by tools/ir-synth/verify_irs.py against resources/irs/manifest.json
// in CI), so a hash of the file's bytes is stable across a rebuild. The
// interesting case is a RETUNE: if a cabinet model is ever adjusted, the sound
// changes. An identity-based id ("guitar-412-cone") would silently follow the
// retuned file, so the preset would keep loading, keep looking correct, and
// quietly recall a different sound - precisely the failure presets exist to
// prevent. A byte hash MISSES after a retune, which degrades loudly instead of
// lying.
//
// The human-readable name travelling next to the hash is for the message shown
// when a reference misses, and for nothing else. Resolution is BY HASH ONLY -
// see basilica::ir::IrContentIndex. Nothing in this file or its callers ever
// looks an IR up by name, because a name lookup is exactly the silent-wrong-
// sound failure the hash is chosen to avoid.
//
// THREADING. Hashing opens and reads a file: message thread / background
// thread only, never processBlock().
namespace basilica::presets
{
    // One slot's reference. `contentHash` empty means "this preset does not
    // reference an IR for this slot", which is the default and is not an
    // error.
    struct IrReference
    {
        // Lowercase SHA-256 hex of the IR file's bytes, as produced by
        // contentHashOfFile(). The same digest resources/irs/manifest.json
        // records per bundled IR, so a factory preset's reference can be
        // pasted straight out of the manifest and checked by eye.
        juce::String contentHash;

        // Display only. NEVER used to find a file - see the file-level note.
        juce::String displayName;

        bool isPresent() const noexcept { return contentHash.isNotEmpty(); }
    };

    // Nave has two independent IR slots (see ParamIDs::irFilePathProperty /
    // irFilePathBProperty); either, both or neither may be referenced.
    struct PresetIrReferences
    {
        IrReference slotA;
        IrReference slotB;

        bool isPresent() const noexcept { return slotA.isPresent() || slotB.isPresent(); }
    };

    //==========================================================================
    // JSON representation, as an OPTIONAL top-level object on the existing
    // "basilica-preset-1" document:
    //
    //   {
    //     "format": "basilica-preset-1",     <- deliberately unchanged
    //     ...
    //     "ir": {
    //       "a": { "sha256": "<64 hex chars>", "name": "Modelled 4x12 Ceramic Cone" },
    //       "b": { "sha256": "<64 hex chars>", "name": "Modelled 4x12 Room 1m" }
    //     }
    //   }
    //
    // THE FORMAT TAG DOES NOT CHANGE, AND THAT IS THE WHOLE BACKWARD-
    // COMPATIBILITY STORY. PresetManager::parseAndValidate() rejects a file
    // whose "format" is not exactly presetFormatTag and accepts every unknown
    // key beyond it, so a build that predates this file reads a preset
    // carrying "ir" as a perfectly ordinary preset and ignores the object it
    // does not understand. Bumping the tag to "basilica-preset-2" would have
    // made every older build refuse the file outright - a hard failure to open,
    // rather than the graceful degradation the format needs. See
    // tests/PresetIrReferenceTests.cpp, which pins both halves.
    inline constexpr const char* irReferenceKey = "ir";
    inline constexpr const char* irReferenceSlotAKey = "a";
    inline constexpr const char* irReferenceSlotBKey = "b";
    inline constexpr const char* irReferenceHashKey = "sha256";
    inline constexpr const char* irReferenceNameKey = "name";

    // Reads the optional "ir" object off a parsed preset document. A preset
    // without one (every preset written before this feature, and every preset
    // saved with no IR loaded) yields a default-constructed, absent result -
    // never an error.
    PresetIrReferences readIrReferences (const juce::var& presetObject);

    // Writes the "ir" object onto a preset document being built. Writes
    // NOTHING when neither slot is referenced, so a preset with no IR loaded
    // is byte-identical to what the pre-#42 saver produced.
    void writeIrReferences (juce::DynamicObject& presetObject, const PresetIrReferences& references);

    //==========================================================================
    // Lowercase SHA-256 hex of the file's bytes, or an empty string if the
    // file does not exist or cannot be read. Blocking file I/O - never call
    // from the audio thread.
    juce::String contentHashOfFile (const juce::File& file);

    // The name carried alongside the hash when Nave saves a preset: the file's
    // stem with underscores turned into spaces and each word capitalised, so
    // "modelled_4x12_ceramic_cone.wav" reads back as "Modelled 4x12 Ceramic
    // Cone" - the same shape as the display_name entries in
    // resources/irs/manifest.json. Purely cosmetic; nothing resolves by it.
    //
    // The juce::String overload takes a bare FILE NAME (not a path), for
    // callers holding an embedded asset's name rather than a file on disk.
    juce::String displayNameForIrFileName (const juce::String& fileName);
    juce::String displayNameForIrFile (const juce::File& file);
}
