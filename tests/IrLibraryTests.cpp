#include "PluginProcessor.h"
#include "TestHelpers.h"
#include "ir/IrLibrary.h"
#include "params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>

// Tests for the IR browser's non-GUI backbone (src/ir/IrLibrary.h): the
// synchronous directory scan, the background scanner's deliver/supersede
// lifecycle, and the library-folder state property's round-trip through the
// processor. The browser overlay itself is covered in
// tests/gui/IrBrowserPanelTests.cpp.
namespace
{
    using basilica::ir::IrLibrary::scan;
    using basilica::ir::IrLibraryScanner;

    // A self-deleting temp directory for scan fixtures.
    struct TempDir
    {
        TempDir()
            : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getChildFile ("nave-ir-library-tests")
                        .getNonexistentSibling())
        {
            REQUIRE (root.createDirectory().wasOk());
        }

        ~TempDir() { root.deleteRecursively(); }

        // Creates a file with a few bytes of throwaway content (the scan
        // only looks at names/extensions, not audio validity).
        juce::File makeFile (const juce::String& relativePath) const
        {
            auto file = root.getChildFile (relativePath);
            REQUIRE (file.create().wasOk());
            REQUIRE (file.replaceWithText ("x"));
            return file;
        }

        juce::File root;
    };
}

TEST_CASE ("isImpulseResponseFile accepts WAV/AIFF in any case and rejects everything else", "[ir-library]")
{
    using basilica::ir::IrLibrary::isImpulseResponseFile;

    CHECK (isImpulseResponseFile (juce::File ("/x/cab.wav")));
    CHECK (isImpulseResponseFile (juce::File ("/x/CAB.WAV")));
    CHECK (isImpulseResponseFile (juce::File ("/x/cab.aif")));
    CHECK (isImpulseResponseFile (juce::File ("/x/cab.Aiff")));

    CHECK_FALSE (isImpulseResponseFile (juce::File ("/x/cab.txt")));
    CHECK_FALSE (isImpulseResponseFile (juce::File ("/x/cab.flac")));
    CHECK_FALSE (isImpulseResponseFile (juce::File ("/x/cab.wav.bak")));
    CHECK_FALSE (isImpulseResponseFile (juce::File ("/x/wav"))); // extensionless
}

TEST_CASE ("scan finds IR files recursively, filters non-IRs and hidden files, and sorts deterministically", "[ir-library]")
{
    TempDir dir;

    // Created deliberately out of sorted order.
    const auto deep = dir.makeFile ("subA/deeper/two.WAV");
    const auto top = dir.makeFile ("one.wav");
    const auto aiff = dir.makeFile ("subB/three.aiff");
    const auto aif = dir.makeFile ("four.aif");
    dir.makeFile ("notes.txt");        // wrong type - excluded
    dir.makeFile (".hidden.wav");      // dot-prefixed - excluded on EVERY
                                       // platform (on Windows a dotfile is
                                       // NOT natively hidden; the scan
                                       // skips it explicitly - see
                                       // IrLibrary.cpp's AppleDouble note)
    dir.makeFile ("subA/._two.WAV");   // macOS AppleDouble sidecar - excluded

    const auto results = scan (dir.root);

    REQUIRE (results.size() == 4);

    // Sorted case-insensitively by full path: four.aif < one.wav <
    // subA/deeper/two.WAV < subB/three.aiff (all under the same root).
    CHECK (results[0] == aif);
    CHECK (results[1] == top);
    CHECK (results[2] == deep);
    CHECK (results[3] == aiff);

    // A second scan of the same tree returns the identical ordering.
    CHECK (scan (dir.root) == results);
}

TEST_CASE ("scan of a missing or non-directory root yields an empty result", "[ir-library]")
{
    TempDir dir;
    const auto file = dir.makeFile ("cab.wav");

    CHECK (scan (dir.root.getChildFile ("does-not-exist")).isEmpty());
    CHECK (scan (file).isEmpty()); // a file, not a directory
}

TEST_CASE ("scan honours the maxFiles cap and the abort hook", "[ir-library]")
{
    TempDir dir;

    for (int i = 0; i < 10; ++i)
        dir.makeFile ("ir" + juce::String (i) + ".wav");

    CHECK (scan (dir.root, 3).size() == 3);
    CHECK (scan (dir.root, 0).isEmpty());

    // An aborted scan returns nothing rather than a partial listing.
    CHECK (scan (dir.root, 100, [] { return true; }).isEmpty());
}

TEST_CASE ("IrLibraryScanner delivers scan results off-thread, stamped with the request generation", "[ir-library]")
{
    TempDir dir;
    const auto expected = dir.makeFile ("cab.wav");

    IrLibraryScanner scanner;

    juce::WaitableEvent delivered;
    juce::Array<juce::File> deliveredFiles;
    juce::File deliveredRoot;
    int deliveredGeneration = 0;

    // onScanFinished runs on the scanner thread - safe to write these
    // captures directly because the test only reads them after wait()
    // returns (WaitableEvent::signal/wait is the synchronisation point).
    scanner.onScanFinished = [&] (juce::Array<juce::File> files, juce::File scannedRoot, int generation)
    {
        deliveredFiles = std::move (files);
        deliveredRoot = scannedRoot;
        deliveredGeneration = generation;
        delivered.signal();
    };

    const auto requestedGeneration = scanner.startScan (dir.root);

    REQUIRE (delivered.wait (5000));
    CHECK (deliveredGeneration == requestedGeneration);
    CHECK (deliveredGeneration == scanner.latestGeneration());
    CHECK (deliveredRoot == dir.root);
    REQUIRE (deliveredFiles.size() == 1);
    CHECK (deliveredFiles[0] == expected);
}

TEST_CASE ("IrLibraryScanner: a restarted scan supersedes the previous request", "[ir-library]")
{
    TempDir dirOne;
    TempDir dirTwo;
    dirOne.makeFile ("first.wav");
    const auto second = dirTwo.makeFile ("second.wav");

    IrLibraryScanner scanner;

    juce::CriticalSection deliveryLock;
    juce::Array<juce::File> latestFiles;
    int latestDeliveredGeneration = 0;
    juce::WaitableEvent sawLatestGeneration;

    scanner.onScanFinished = [&] (juce::Array<juce::File> files, juce::File, int generation)
    {
        const juce::ScopedLock scopedLock (deliveryLock);
        latestFiles = std::move (files);
        latestDeliveredGeneration = generation;

        if (generation == scanner.latestGeneration())
            sawLatestGeneration.signal();
    };

    // Two requests in quick succession: the first may or may not complete
    // before it is superseded, but the LAST delivery must be the second
    // request's generation and contents.
    scanner.startScan (dirOne.root);
    const auto finalGeneration = scanner.startScan (dirTwo.root);

    REQUIRE (sawLatestGeneration.wait (5000));

    const juce::ScopedLock scopedLock (deliveryLock);
    CHECK (latestDeliveredGeneration == finalGeneration);
    REQUIRE (latestFiles.size() == 1);
    CHECK (latestFiles[0] == second);
}

TEST_CASE ("The default library directory lives under the user's music folder", "[ir-library]")
{
    const auto defaultDir = basilica::ir::IrLibrary::defaultDirectory();

    CHECK (defaultDir.getFileName() == "Impulse Responses");
    CHECK (defaultDir.getParentDirectory().getFileName() == "Nave");
    CHECK (defaultDir.isAChildOf (juce::File::getSpecialLocation (juce::File::userMusicDirectory)));

    // Purely a path computation - it must never create the folder as a side
    // effect (see IrLibrary.h's docs).
    // (No CHECK on existence: the user may legitimately have created it.)
}

TEST_CASE ("The IR library folder property round-trips through the processor state", "[ir-library][state]")
{
    TempDir dir;

    NaveAudioProcessor source;
    source.apvts.state.setProperty (ParamIDs::irLibraryFolderProperty,
                                    dir.root.getFullPathName(), nullptr);

    juce::MemoryBlock stateData;
    source.getStateInformation (stateData);

    NaveAudioProcessor restored;
    restored.setStateInformation (stateData.getData(), (int) stateData.getSize());

    CHECK (restored.apvts.state.getProperty (ParamIDs::irLibraryFolderProperty).toString()
           == dir.root.getFullPathName());
}
