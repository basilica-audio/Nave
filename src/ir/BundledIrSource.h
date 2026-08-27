#pragma once

#include "FactoryIrLibrary.h"

#include <juce_core/juce_core.h>

#include <map>
#include <vector>

// Nave's own IRs as a CONTENT-ADDRESSED RESOLUTION SOURCE (issue #45).
//
// WHAT THIS IS FOR. src/ir/IrContentIndex.h answers "is there a file in the
// user's IR library whose bytes hash to this digest". This answers the same
// question of the bytes compiled into the binary, so a preset that references
// a bundled cabinet resolves on a machine where the user has never pressed
// "Install Library". Both answers are keyed by the SAME digest, which is what
// makes them interchangeable rather than merely similar - see the precedence
// note below.
//
// WHY IT MATERIALISES A FILE INSTEAD OF DECODING FROM MEMORY. #45's central
// objection to an embedded resolution path is that it would be a SECOND WAY
// to turn bundled content into engine samples, and two paths to one audible
// outcome drift: one grows a normalisation or a sample-rate conversion the
// other does not, and the preset then sounds different depending on whether
// the library happened to be installed. So there is still exactly one decode
// path. This module does not decode anything. It writes the embedded bytes
// out to a real file and hands that file to the same
// NaveAudioProcessor::loadImpulseResponseFromFile() a browser selection goes
// through. Sample-identity between the two resolution sources is therefore a
// property of the design rather than of a test that has to keep catching a
// regression - the test in tests/BundledIrResolutionTests.cpp pins it anyway,
// because the day someone adds a memory decoder is the day it stops being
// free.
//
// Materialising also keeps ParamIDs::irFilePathProperty an honest absolute
// path to a file that really holds those bytes. Every existing invariant that
// reads it stays true without modification: re-saving the preset re-hashes
// that file and preserves the reference, the "the slot already IS these
// bytes" short-circuit in applyPresetIrReferences() keeps working, and a
// session reload can find the audio again.
//
// PRECEDENCE, AND WHY IT IS NOT AUDIBLE. The user's library is consulted
// first and this source only afterwards (see
// NaveAudioProcessor::resolveIrReference). That ordering decides WHERE the
// file comes from and never WHAT IT SOUNDS LIKE: a digest can only match
// bytes equal to it, so when both sources hold the hash they hold the same
// audio. #45 asks whether a mismatch between the sources is worth surfacing;
// there is no such thing as a mismatch under a content hash, which is exactly
// why the hash was the right identifier in #42.
//
// NOT A LIBRARY. Nothing here is scanned, listed or offered in the IR
// browser. The browser lists files, and "Install Library" remains the single
// act that puts the bundled cabinets where it can list them. This source only
// ever answers an EXPLICIT reference to a specific digest.
//
// THREADING. Hashing and file writes: message thread (or a background
// thread), never processBlock(). Not internally synchronised.
namespace basilica::ir
{
    class BundledIrSource
    {
    public:
        // Indexes the AUDIO assets in `assets` by the SHA-256 of their bytes.
        // Non-audio assets (LICENSES.md, CC0-1.0.txt, manifest.json) are
        // skipped: they travel with the library for provenance, they are not
        // cabinets, and indexing them would let a preset "resolve" to a text
        // file.
        //
        // LIFETIME: keeps pointers into `assets`, which must outlive this
        // object. The intended argument is nave::factoryIrAssets(), a
        // function-local static.
        explicit BundledIrSource (const std::vector<FactoryIrAsset>& assets);

        // The embedded asset whose bytes hash to `contentHash` (lowercase
        // hex), or nullptr. An empty or malformed digest is a miss, not an
        // error. O(log n) - the digests are computed once, at construction.
        const FactoryIrAsset* findByContentHash (const juce::String& contentHash) const;

        // The display name of the asset with that digest, or an empty string
        // when the digest is not one of Nave's own. Derived from the embedded
        // file NAME rather than from manifest.json, so a message can never
        // claim an IR ships on the strength of a manifest entry whose audio
        // did not.
        juce::String displayNameForContentHash (const juce::String& contentHash) const;

        // Writes `asset` into `cacheFolder` under its own file name and
        // returns the file, or a default juce::File if it could not be
        // produced (unwritable location, full disk, unusable asset). The
        // failure is a normal result and the caller must treat it as a miss -
        // see #45's requirement that every lookup has a defined outcome.
        //
        // Idempotent: a byte-identical file already sitting there is reused
        // without a write, and a truncated or edited one is rewritten. Both
        // via FactoryIrLibrary, so this cannot drift from what
        // "Install Library" produces.
        juce::File materialise (const FactoryIrAsset& asset, const juce::File& cacheFolder) const;

        // Convenience: find + materialise. Returns a default juce::File on
        // either a miss or a failed write.
        juce::File materialiseByContentHash (const juce::String& contentHash,
                                             const juce::File& cacheFolder) const;

        // Lowercase SHA-256 hex of an asset's embedded bytes, or an empty
        // string for an unusable asset. The same digest
        // resources/irs/manifest.json records for that file.
        static juce::String contentHashOf (const FactoryIrAsset& asset);

        // True for the asset file names this source treats as cabinets. Kept
        // in step with IrLibrary::isImpulseResponseFile(), which asks the
        // same question of a file on disk.
        static bool isAudioAssetName (const juce::String& fileName);

        // Every indexed digest, for tests and diagnostics.
        std::vector<juce::String> allContentHashes() const;

    private:
        // Keyed by lowercase hex digest. std::map rather than unordered_map
        // for the same reason IrContentIndex uses one: nine entries, never on
        // a hot path, and readable in a debugger.
        std::map<juce::String, const FactoryIrAsset*> byDigest;
    };
}
