#include "FactoryIrLibrary.h"

#include <cstring>

namespace basilica::ir::FactoryIrLibrary
{
    namespace
    {
        bool isUsable (const FactoryIrAsset& asset) noexcept
        {
            return asset.fileName != nullptr
                && asset.data != nullptr
                && asset.dataSize > 0;
        }
    }

    AssetState stateOf (const juce::File& destinationFolder, const FactoryIrAsset& asset)
    {
        if (! isUsable (asset))
            return AssetState::missing;

        const auto file = destinationFolder.getChildFile (asset.fileName);

        if (! file.existsAsFile())
            return AssetState::missing;

        // Size first: a mismatch settles it without reading anything, which
        // is what keeps isInstalledIn() cheap enough to call every time the
        // browser opens.
        if (file.getSize() != static_cast<juce::int64> (asset.dataSize))
            return AssetState::differs;

        juce::MemoryBlock onDisk;

        if (! file.loadFileAsData (onDisk))
            return AssetState::differs;

        if (onDisk.getSize() != static_cast<size_t> (asset.dataSize))
            return AssetState::differs;

        return std::memcmp (onDisk.getData(), asset.data, static_cast<size_t> (asset.dataSize)) == 0
                   ? AssetState::matches
                   : AssetState::differs;
    }

    bool isInstalledIn (const juce::File& destinationFolder,
                        const std::vector<FactoryIrAsset>& assets)
    {
        if (assets.empty() || ! destinationFolder.isDirectory())
            return false;

        for (const auto& asset : assets)
            if (stateOf (destinationFolder, asset) != AssetState::matches)
                return false;

        return true;
    }

    juce::String InstallResult::summary() const
    {
        if (! succeeded())
            return "Could not install " + juce::String (failures.size())
                 + (failures.size() == 1 ? " file: " : " files: ")
                 + failures.joinIntoString (", ");

        if (written == 0)
            return "The bundled library is already installed";

        return "Installed " + juce::String (written)
             + (written == 1 ? " file" : " files");
    }

    InstallResult installInto (const juce::File& destinationFolder,
                               const std::vector<FactoryIrAsset>& assets)
    {
        InstallResult result;

        if (const auto created = destinationFolder.createDirectory(); ! created.wasOk())
        {
            // One failure entry rather than one per asset: the folder is the
            // single cause, and repeating it nine times would push the real
            // message out of the browser's status row.
            result.failures.add (destinationFolder.getFullPathName()
                                     + " (" + created.getErrorMessage() + ")");
            return result;
        }

        for (const auto& asset : assets)
        {
            if (! isUsable (asset))
            {
                // A null/empty blob means the BinaryData wiring in
                // CMakeLists.txt and the asset list in PluginEditor.cpp have
                // drifted apart. Reported rather than skipped silently, so
                // the install does not claim success while shipping less
                // than it promised.
                result.failures.add (asset.fileName != nullptr ? juce::String (asset.fileName)
                                                               : juce::String ("<unnamed asset>"));
                continue;
            }

            if (stateOf (destinationFolder, asset) == AssetState::matches)
            {
                ++result.alreadyPresent;
                continue;
            }

            const auto file = destinationFolder.getChildFile (asset.fileName);

            if (file.replaceWithData (asset.data, static_cast<size_t> (asset.dataSize)))
                ++result.written;
            else
                result.failures.add (asset.fileName);
        }

        return result;
    }
}
