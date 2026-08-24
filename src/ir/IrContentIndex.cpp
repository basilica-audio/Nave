#include "IrContentIndex.h"

#include "IrLibrary.h"
#include "../presets/IrReference.h"

#include <algorithm>

namespace basilica::ir
{
    void IrContentIndex::setSearchRoots (std::vector<juce::File> rootsToSearch)
    {
        roots.clear();

        for (auto& root : rootsToSearch)
        {
            if (root == juce::File() || ! root.isDirectory())
                continue;

            // The default library folder and a user-chosen one are frequently
            // the same directory; scanning it twice would double the work and
            // could report a "first" match that depends on which duplicate ran
            // first.
            const auto alreadyPresent = std::find (roots.begin(), roots.end(), root) != roots.end();

            if (! alreadyPresent)
                roots.push_back (root);
        }
    }

    void IrContentIndex::clearCache()
    {
        digestCache.clear();
    }

    juce::File IrContentIndex::findByContentHash (const juce::String& contentHash)
    {
        const auto wanted = contentHash.trim().toLowerCase();

        if (wanted.length() != 64 || ! wanted.containsOnly ("0123456789abcdef"))
            return {};

        for (const auto& root : roots)
        {
            for (const auto& candidate : IrLibrary::scan (root))
            {
                const auto path = candidate.getFullPathName();
                const auto size = candidate.getSize();
                const auto modified = candidate.getLastModificationTime().toMilliseconds();

                auto cached = digestCache.find (path);

                if (cached == digestCache.end()
                    || cached->second.sizeInBytes != size
                    || cached->second.modificationTimeMs != modified)
                {
                    const auto hash = presets::contentHashOfFile (candidate);

                    if (hash.isEmpty())
                        continue; // unreadable/vanished between the scan and now

                    cached = digestCache.insert_or_assign (path, CachedDigest { size, modified, hash }).first;
                }

                if (cached->second.hash == wanted)
                    return candidate;
            }
        }

        return {};
    }
}
