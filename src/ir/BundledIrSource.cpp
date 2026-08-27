#include "BundledIrSource.h"

#include "../presets/IrReference.h"

#include <juce_cryptography/juce_cryptography.h>

namespace basilica::ir
{
    BundledIrSource::BundledIrSource (const std::vector<FactoryIrAsset>& assets)
    {
        for (const auto& asset : assets)
        {
            if (asset.fileName == nullptr)
                continue;

            if (! isAudioAssetName (asset.fileName))
                continue;

            const auto digest = contentHashOf (asset);

            if (digest.isEmpty())
                continue;

            // insert(), not insert_or_assign(): if two embedded assets ever
            // held identical bytes the first one wins, which keeps the answer
            // independent of the order nave::factoryIrAssets() happens to list
            // them in.
            byDigest.insert ({ digest, &asset });
        }
    }

    bool BundledIrSource::isAudioAssetName (const juce::String& fileName)
    {
        return fileName.endsWithIgnoreCase (".wav")
            || fileName.endsWithIgnoreCase (".aif")
            || fileName.endsWithIgnoreCase (".aiff");
    }

    juce::String BundledIrSource::contentHashOf (const FactoryIrAsset& asset)
    {
        if (asset.data == nullptr || asset.dataSize <= 0)
            return {};

        return juce::SHA256 (asset.data, static_cast<size_t> (asset.dataSize)).toHexString().toLowerCase();
    }

    const FactoryIrAsset* BundledIrSource::findByContentHash (const juce::String& contentHash) const
    {
        // The same validation IrContentIndex::findByContentHash() applies, so
        // a malformed digest misses identically in both sources rather than
        // misbehaving differently depending on which one is asked.
        const auto wanted = contentHash.trim().toLowerCase();

        if (wanted.length() != 64 || ! wanted.containsOnly ("0123456789abcdef"))
            return nullptr;

        const auto found = byDigest.find (wanted);

        return found == byDigest.end() ? nullptr : found->second;
    }

    juce::String BundledIrSource::displayNameForContentHash (const juce::String& contentHash) const
    {
        if (const auto* asset = findByContentHash (contentHash))
            return presets::displayNameForIrFileName (asset->fileName);

        return {};
    }

    juce::File BundledIrSource::materialise (const FactoryIrAsset& asset, const juce::File& cacheFolder) const
    {
        if (asset.fileName == nullptr || asset.data == nullptr || asset.dataSize <= 0)
            return {};

        if (cacheFolder == juce::File())
            return {};

        // Routed through FactoryIrLibrary rather than reimplemented: it is the
        // module that already knows how to put an embedded asset on disk
        // exactly once (size-then-bytes comparison, atomic replaceWithData),
        // and sharing it means a materialised file and an installed one can
        // never differ.
        const auto result = FactoryIrLibrary::installInto (cacheFolder, { asset });

        if (! result.succeeded())
            return {};

        const auto file = cacheFolder.getChildFile (asset.fileName);

        // Verified rather than assumed: installInto() reports on the write,
        // and this is the claim the caller actually needs - that the file is
        // there and is the referenced bytes. A caller that got a File back for
        // something that vanished between the write and the load would report
        // "resolved" for a slot it never filled.
        return FactoryIrLibrary::stateOf (cacheFolder, asset) == FactoryIrLibrary::AssetState::matches
                   ? file
                   : juce::File();
    }

    juce::File BundledIrSource::materialiseByContentHash (const juce::String& contentHash,
                                                          const juce::File& cacheFolder) const
    {
        if (const auto* asset = findByContentHash (contentHash))
            return materialise (*asset, cacheFolder);

        return {};
    }

    std::vector<juce::String> BundledIrSource::allContentHashes() const
    {
        std::vector<juce::String> digests;
        digests.reserve (byDigest.size());

        for (const auto& entry : byDigest)
            digests.push_back (entry.first);

        return digests;
    }
}
