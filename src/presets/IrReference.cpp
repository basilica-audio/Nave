#include "IrReference.h"

#include <juce_cryptography/juce_cryptography.h>

namespace basilica::presets
{
    namespace
    {
        IrReference readSlot (const juce::var& irObject, const char* slotKey)
        {
            auto* obj = irObject.getDynamicObject();

            if (obj == nullptr)
                return {};

            const auto slotVar = obj->getProperty (slotKey);
            auto* slotObj = slotVar.getDynamicObject();

            if (slotObj == nullptr)
                return {};

            IrReference reference;
            reference.contentHash = slotObj->getProperty (irReferenceHashKey).toString().trim().toLowerCase();
            reference.displayName = slotObj->getProperty (irReferenceNameKey).toString();

            // A malformed hash is treated as no reference rather than as a
            // reference that can never resolve: the former loads the preset
            // silently (correct - the field is optional), the latter would
            // nag the user about a file that was never named properly in the
            // first place.
            if (reference.contentHash.length() != 64
                || ! reference.contentHash.containsOnly ("0123456789abcdef"))
                return {};

            return reference;
        }

        void writeSlot (juce::DynamicObject& irObject, const char* slotKey, const IrReference& reference)
        {
            if (! reference.isPresent())
                return;

            auto* slotObj = new juce::DynamicObject();
            slotObj->setProperty (irReferenceHashKey, reference.contentHash);
            slotObj->setProperty (irReferenceNameKey, reference.displayName);

            irObject.setProperty (slotKey, juce::var (slotObj));
        }
    }

    //==========================================================================
    PresetIrReferences readIrReferences (const juce::var& presetObject)
    {
        auto* obj = presetObject.getDynamicObject();

        if (obj == nullptr || ! obj->hasProperty (irReferenceKey))
            return {};

        const auto irObject = obj->getProperty (irReferenceKey);

        PresetIrReferences references;
        references.slotA = readSlot (irObject, irReferenceSlotAKey);
        references.slotB = readSlot (irObject, irReferenceSlotBKey);
        return references;
    }

    void writeIrReferences (juce::DynamicObject& presetObject, const PresetIrReferences& references)
    {
        if (! references.isPresent())
            return;

        auto* irObject = new juce::DynamicObject();
        writeSlot (*irObject, irReferenceSlotAKey, references.slotA);
        writeSlot (*irObject, irReferenceSlotBKey, references.slotB);

        presetObject.setProperty (irReferenceKey, juce::var (irObject));
    }

    //==========================================================================
    juce::String contentHashOfFile (const juce::File& file)
    {
        if (! file.existsAsFile())
            return {};

        // juce::SHA256 (const File&) (JUCE 8.0.14, juce_cryptography) opens
        // its own FileInputStream and streams the whole file, so nothing is
        // read into memory whole. Blocking I/O - message thread only.
        return juce::SHA256 (file).toHexString().toLowerCase();
    }

    juce::String displayNameForIrFile (const juce::File& file)
    {
        return displayNameForIrFileName (file.getFileName());
    }

    juce::String displayNameForIrFileName (const juce::String& fileName)
    {
        const auto stem = fileName.upToLastOccurrenceOf (".", false, false);
        const auto words = juce::StringArray::fromTokens (
            (stem.isEmpty() ? fileName : stem).replaceCharacter ('_', ' '), " ", {});

        juce::StringArray capitalised;

        for (const auto& word : words)
        {
            if (word.isEmpty())
                continue;

            // Only the first character is touched: "4x10" must stay "4x10",
            // and a user's own "SM57" must not become "Sm57".
            capitalised.add (word.substring (0, 1).toUpperCase() + word.substring (1));
        }

        return capitalised.joinIntoString (" ");
    }
}
