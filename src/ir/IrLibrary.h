#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>

// The IR browser's non-GUI backbone (issue #1, "IR browser"): pure directory-
// scanning helpers plus a background scanner thread, kept free of any
// juce_gui_basics dependency so tests/IrLibraryTests.cpp can exercise the
// whole scan/supersede/deliver lifecycle headlessly, and so sibling plugins
// can reuse it without dragging in Nave's editor.
//
// THREADING. Everything here runs OFF the audio thread by construction:
// scanning is file-system I/O on IrLibraryScanner's own background thread,
// and the results are handed to the GUI via a callback that the *consumer*
// is responsible for trampolining onto the message thread (see
// IrLibraryScanner::onScanFinished's docs and IrBrowserPanel.cpp for the
// SafePointer + MessageManager::callAsync pattern). Nothing in this file is
// ever called from processBlock(); the actual IR *load* that follows a
// browser selection goes through NaveAudioProcessor::
// loadImpulseResponseFromFile() on the message thread, whose convolver
// hand-off is juce::dsp::Convolution::loadImpulseResponse() - documented in
// JUCE 8.0.14 as wait-free, with the IR prepared asynchronously on
// Convolution's background thread and swapped in once ready.
namespace basilica::ir
{
    namespace IrLibrary
    {
        // Upper bound on how many files a single scan collects. Guards the
        // browser (and the scan itself) against a user pointing the library
        // folder at, say, their entire home directory - 2000 rows is already
        // far beyond what a flat list browser is useful for, and the cap
        // keeps worst-case scan cost and memory bounded and predictable.
        inline constexpr int defaultMaxFiles = 2000;

        // The out-of-the-box library folder the browser scans before the
        // user picks their own: <user music dir>/Nave/Impulse Responses
        // (e.g. ~/Music/Nave/Impulse Responses on macOS). Deliberately NOT
        // created here - defaultDirectory() is a pure path computation, and
        // a scan of a not-yet-existing folder simply yields an empty list
        // (see scan() below), which the browser presents with a "choose a
        // folder" hint instead of silently creating directories the user
        // never asked for.
        juce::File defaultDirectory();

        // Where Nave writes its own embedded IRs out to disk so that a preset
        // reference can resolve against them without the user having pressed
        // "Install Library" (issue #45, see src/ir/BundledIrSource.h):
        // <user app data>/Basilica Audio/Nave/Bundled Impulse Responses
        // (on macOS, <user app data> is ~/Library/Application Support).
        //
        // DELIBERATELY NOT defaultDirectory(). Writing there would silently
        // perform the install the user did not ask for, which is precisely
        // what src/ir/FactoryIrLibrary.h refuses to do, and it would put files
        // the user never chose into the folder the browser lists. This
        // location is NEVER SCANNED and never appears in the browser: it is a
        // reconstructible cache of bytes that are already inside the binary,
        // not a library. Deleting it costs nothing - the next reference that
        // needs a file re-creates it.
        //
        // Like defaultDirectory(), a pure path computation that creates
        // nothing.
        juce::File bundledCacheDirectory();

        // True for the audio-file types the IR slots can actually load
        // (WAV/AIFF - the same "*.wav;*.aiff;*.aif" filter the editor's
        // direct file-chooser path uses), matched case-insensitively.
        bool isImpulseResponseFile (const juce::File& file);

        // Recursively collects every IR file under `root` (skipping hidden
        // entries), sorted case-insensitively by full path so the listing
        // order is deterministic across platforms and repeat scans, capped
        // at `maxFiles`. A non-existent/non-directory `root` yields an
        // empty result. `shouldAbort` (optional) is polled once per
        // directory entry so a superseded/stopping background scan can bail
        // out early instead of finishing a walk nobody wants anymore;
        // an aborted scan returns an empty (never partial) result.
        //
        // Synchronous and blocking - call it from a background thread (see
        // IrLibraryScanner) or a test, never from the audio thread.
        juce::Array<juce::File> scan (const juce::File& root,
                                      int maxFiles = defaultMaxFiles,
                                      const std::function<bool()>& shouldAbort = {});
    }

    // Runs IrLibrary::scan() on a dedicated background thread so the message
    // thread never blocks on a slow directory walk (network drives, huge
    // sample libraries). Restartable: each startScan() supersedes any scan
    // still in flight - a superseded scan's results are discarded (never
    // delivered), and the newest requested root is scanned instead.
    //
    // Call startScan() from one thread at a time (in practice: the message
    // thread). Set onScanFinished before the first startScan() call.
    class IrLibraryScanner final : private juce::Thread
    {
    public:
        IrLibraryScanner() : juce::Thread ("Nave IR library scan") {}

        ~IrLibraryScanner() override
        {
            // Generous timeout, then hard-kill as a last resort - the scan
            // polls threadShouldExit() every directory entry, so in practice
            // it exits within one file-system call.
            stopThread (5000);
        }

        // Called ON THE SCANNER THREAD when a scan completes and is still
        // the newest request. Consumers that touch Components must hop to
        // the message thread themselves (MessageManager::callAsync +
        // Component::SafePointer, see IrBrowserPanel.cpp) and should
        // re-check `generation` against latestGeneration() after the hop,
        // in case yet another scan was requested while the message was in
        // flight.
        std::function<void (juce::Array<juce::File> files,
                            juce::File scannedRoot,
                            int generation)> onScanFinished;

        // Requests a (re)scan of `rootToScan`, superseding any scan in
        // flight, and returns this request's generation stamp.
        int startScan (const juce::File& rootToScan)
        {
            {
                const juce::ScopedLock scopedLock (requestLock);
                requestedRoot = rootToScan;
            }

            const auto generation = ++requestedGeneration;

            // Safe to call repeatedly - juce::Thread::startThread() is a
            // no-op when the thread is already running, and notify() sets
            // the internal event even when run() is not currently in
            // wait(), so a request can never be missed between the two.
            startThread();
            notify();

            return generation;
        }

        // The most recently requested scan's generation stamp.
        int latestGeneration() const noexcept { return requestedGeneration.load(); }

    private:
        void run() override
        {
            while (! threadShouldExit())
            {
                wait (-1);

                // Inner loop: if a new request lands mid-scan, discard and
                // rescan immediately instead of bouncing through wait().
                while (! threadShouldExit())
                {
                    const auto generation = requestedGeneration.load();

                    juce::File root;
                    {
                        const juce::ScopedLock scopedLock (requestLock);
                        root = requestedRoot;
                    }

                    auto files = IrLibrary::scan (root, IrLibrary::defaultMaxFiles,
                                                  [this] { return threadShouldExit(); });

                    if (threadShouldExit())
                        return;

                    if (generation != requestedGeneration.load())
                        continue; // superseded mid-scan - walk the newest root instead

                    if (onScanFinished != nullptr)
                        onScanFinished (std::move (files), root, generation);

                    break;
                }
            }
        }

        juce::CriticalSection requestLock;
        juce::File requestedRoot; // guarded by requestLock
        std::atomic<int> requestedGeneration { 0 };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IrLibraryScanner)
    };
}
