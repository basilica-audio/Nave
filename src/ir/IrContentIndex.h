#pragma once

#include <juce_core/juce_core.h>

#include <map>
#include <vector>

// Finding an impulse-response file BY ITS BYTES (issue #42).
//
// This is the resolution half of src/presets/IrReference.h: a preset records
// the SHA-256 of the IR it was made with, and this walks the user's IR library
// looking for a file whose bytes hash to that digest. There is deliberately no
// lookup by file name, by folder, or by any other identity - a name match on
// bytes that have since changed is exactly the silent-wrong-sound failure the
// content hash exists to prevent, so the only question this class can answer
// is "is there a file here that IS those bytes".
//
// A MISS IS A NORMAL RESULT, not an error: the user may never have installed
// the bundled library, may have moved their folder, or may be opening a preset
// made against an IR they do not own. The caller's job is then to leave the
// currently loaded IR alone and say what was expected - never to substitute
// something else.
//
// COST. A lookup scans the search roots (basilica::ir::IrLibrary::scan, capped
// at IrLibrary::defaultMaxFiles) and hashes candidates. Digests are cached per
// file and reused while the file's size and modification time are unchanged,
// so a second lookup - the common case, since a preset can reference two slots
// and a user auditions several presets in a row - re-reads nothing.
//
// THREADING. Directory walks and file reads throughout: message thread (or a
// background thread) only, never processBlock(). Not internally synchronised;
// use one instance from one thread.
namespace basilica::ir
{
    class IrContentIndex
    {
    public:
        IrContentIndex() = default;

        // The folders searched, in order, by findByContentHash(). Replacing
        // the roots does NOT drop the digest cache: the cache is keyed by
        // file path plus size plus modification time, so entries for files
        // still present under the new roots stay valid and entries for files
        // that are gone simply never get consulted again.
        void setSearchRoots (std::vector<juce::File> rootsToSearch);

        const std::vector<juce::File>& getSearchRoots() const noexcept { return roots; }

        // The first file under the search roots whose contents hash to
        // `contentHash` (lowercase hex), or a default juce::File if there is
        // none. Root order and IrLibrary::scan()'s sorted output make the
        // answer deterministic when the same bytes exist in more than one
        // place.
        //
        // An empty or malformed `contentHash` always yields a miss without
        // touching the file system.
        juce::File findByContentHash (const juce::String& contentHash);

        // Drops every cached digest. Only needed when files may have been
        // rewritten without their size or modification time changing (a
        // pathological case); ordinary edits, installs and deletions are
        // already detected by the cache key.
        void clearCache();

    private:
        struct CachedDigest
        {
            juce::int64 sizeInBytes = 0;
            juce::int64 modificationTimeMs = 0;
            juce::String hash;
        };

        // Keyed by absolute path. std::map rather than an unordered_map so a
        // debug walk of the cache is readable; the map is small (one entry
        // per IR file the user owns) and never on a hot path.
        std::map<juce::String, CachedDigest> digestCache;
        std::vector<juce::File> roots;
    };
}
