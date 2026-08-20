#include "IrLibrary.h"

#include <algorithm>

namespace basilica::ir::IrLibrary
{
    juce::File defaultDirectory()
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                    .getChildFile ("Nave")
                    .getChildFile ("Impulse Responses");
    }

    bool isImpulseResponseFile (const juce::File& file)
    {
        // hasFileExtension() takes a semicolon-separated list and matches
        // case-insensitively (JUCE 8.0.14 juce_File.h), so this single call
        // covers .wav/.WAV/.aif/.aiff and every case mix in between.
        return file.hasFileExtension ("wav;aif;aiff");
    }

    juce::Array<juce::File> scan (const juce::File& root,
                                  int maxFiles,
                                  const std::function<bool()>& shouldAbort)
    {
        juce::Array<juce::File> results;

        if (! root.isDirectory() || maxFiles <= 0)
            return results;

        for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*", juce::File::findFiles))
        {
            if (shouldAbort != nullptr && shouldAbort())
                return {}; // aborted scans yield nothing, never a partial listing

            if (entry.isHidden())
                continue;

            const auto file = entry.getFile();

            if (! isImpulseResponseFile (file))
                continue;

            results.add (file);

            if (results.size() >= maxFiles)
                break;
        }

        // Deterministic, platform-independent listing order: the OS's raw
        // directory-iteration order is explicitly unspecified (and differs
        // between APFS/NTFS/ext4), so the browser sorts rather than trusting
        // it. Full-path compare keeps files grouped by their subfolder.
        std::sort (results.begin(), results.end(),
                   [] (const juce::File& a, const juce::File& b)
                   {
                       return a.getFullPathName().compareIgnoreCase (b.getFullPathName()) < 0;
                   });

        return results;
    }
}
