#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "../TestHelpers.h"
#include "gui/IrBrowserPanel.h"
#include "gui/IrCartridgeSlot.h"
#include "ir/FactoryIrLibrary.h"
#include "ir/IrLibrary.h"
#include "params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>

// Tests for the IR browser overlay (src/gui/IrBrowserPanel.h) and its
// editor wiring - same headless conventions as the rest of tests/gui/
// (ScopedJuceInitialiser_GUI installed in TestMain.cpp, onClick()/
// keyPressed() invoked directly because no message loop is running, and
// applyScanResults()/setFilterText() used as the documented deterministic
// seams because both the directory scan and TextEditor change notifications
// are asynchronous). The scanner's own async path is covered in
// tests/IrLibraryTests.cpp.
namespace
{
    struct BrowserFixture
    {
        BrowserFixture()
        {
            processor.prepareToPlay (48000.0, 512);
            editor = std::make_unique<NaveAudioProcessorEditor> (processor);

            panel = dynamic_cast<basilica::gui::IrBrowserPanel*> (editor->findChildWithID ("irBrowser"));
            REQUIRE (panel != nullptr);

            fileList = dynamic_cast<juce::ListBox*> (panel->findChildWithID ("irBrowser.list"));
            REQUIRE (fileList != nullptr);
        }

        // Creates a real, loadable 64-sample delta-impulse WAV in the temp
        // library so onIrChosen's processor load actually succeeds.
        juce::File makeIrFile (const juce::String& relativePath)
        {
            juce::AudioBuffer<float> ir (1, 64);
            ir.clear();
            ir.setSample (0, 0, 1.0f);

            auto file = libraryRoot.root.getChildFile (relativePath);
            REQUIRE (file.getParentDirectory().createDirectory().wasOk());
            REQUIRE (TestHelpers::writeWavFile (file, ir, 48000.0));
            return file;
        }

        // Wave-3: the D1 cartridge slot component replaced the old
        // Browse.../Load IR.../Default button row - its browse gesture is
        // the overlay's entry point (see gui/IrCartridgeSlot.h).
        basilica::gui::IrCartridgeSlot* slotComponentFor (const juce::String& idPrefix)
        {
            const auto title = idPrefix == "irSlotA" ? juce::String ("Impulse Response A")
                                                     : juce::String ("Impulse Response B");

            for (int i = 0; i < editor->getNumChildComponents(); ++i)
                if (auto* slot = dynamic_cast<basilica::gui::IrCartridgeSlot*> (editor->getChildComponent (i)))
                    if (slot->getTitle() == title)
                        return slot;

            return nullptr;
        }

        void browseFromSlot (const juce::String& idPrefix)
        {
            auto* slot = slotComponentFor (idPrefix);
            REQUIRE (slot != nullptr);
            slot->performBrowse();
        }

        struct TempRoot
        {
            TempRoot()
                : root (juce::File::getSpecialLocation (juce::File::tempDirectory)
                            .getChildFile ("nave-ir-browser-tests")
                            .getNonexistentSibling())
            {
                REQUIRE (root.createDirectory().wasOk());
            }

            ~TempRoot() { root.deleteRecursively(); }

            juce::File root;
        };

        TempRoot libraryRoot;
        NaveAudioProcessor processor;
        std::unique_ptr<NaveAudioProcessorEditor> editor;
        basilica::gui::IrBrowserPanel* panel = nullptr;
        juce::ListBox* fileList = nullptr;
    };
}

TEST_CASE ("Each IR slot is a keyboard-operable cartridge with a slot-specific accessible name", "[gui][a11y][ir-browser]")
{
    BrowserFixture fixture;

    for (const auto* idPrefix : { "irSlotA", "irSlotB" })
    {
        auto* slot = fixture.slotComponentFor (idPrefix);
        REQUIRE (slot != nullptr);

        const auto expectedSlotName = juce::String (idPrefix) == "irSlotA"
                                          ? juce::String ("Impulse Response A")
                                          : juce::String ("Impulse Response B");
        CHECK (slot->getTitle() == expectedSlotName);

        // Focusable, and its browse callback (the overlay's entry point,
        // triggered by click/Return/Space/AT-press) is wired.
        CHECK (slot->getWantsKeyboardFocus());
        REQUIRE (slot->onBrowse);
    }
}

TEST_CASE ("Browse opens the overlay above the faceplate, titled for the clicked slot", "[gui][ir-browser]")
{
    BrowserFixture fixture;

    CHECK_FALSE (fixture.panel->isVisible());

    fixture.browseFromSlot ("irSlotB");

    CHECK (fixture.panel->isVisible());
    CHECK (fixture.panel->getTitle().contains ("IR B"));
    CHECK (fixture.panel->getBounds() == fixture.editor->getLocalBounds());

    // Re-opening for the other slot retargets the same panel instance.
    fixture.browseFromSlot ("irSlotA");
    CHECK (fixture.panel->getTitle().contains ("IR A"));
}

TEST_CASE ("Selecting a row auditions it into the target slot; Return loads and dismisses", "[gui][ir-browser]")
{
    BrowserFixture fixture;

    const auto irOne = fixture.makeIrFile ("cabs/one.wav");
    const auto irTwo = fixture.makeIrFile ("cabs/two.wav");

    fixture.browseFromSlot ("irSlotA");

    // Deterministic seam for the async scan (see file-header comment).
    fixture.panel->applyScanResults ({ irOne, irTwo }, fixture.libraryRoot.root);
    REQUIRE (fixture.panel->getNumVisibleFiles() == 2);
    CHECK (fixture.panel->displayNameForRow (0) == juce::String ("cabs") + juce::File::getSeparatorString() + "one.wav");

    // Single selection = audition: loads immediately, browser stays open.
    fixture.fileList->selectRow (0);
    CHECK (fixture.processor.getCurrentIrFilePath() == irOne.getFullPathName());
    CHECK (fixture.panel->isVisible());

    // Arrowing on (selecting the next row) auditions the next IR.
    fixture.fileList->selectRow (1);
    CHECK (fixture.processor.getCurrentIrFilePath() == irTwo.getFullPathName());

    // Return = commit: keeps the loaded IR and dismisses the overlay.
    juce::Component& listAsComponent = *fixture.fileList;
    REQUIRE (listAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
    CHECK (fixture.processor.getCurrentIrFilePath() == irTwo.getFullPathName());
    CHECK_FALSE (fixture.panel->isVisible());

    // IR B was never touched.
    CHECK (fixture.processor.getCurrentIrFilePathB().isEmpty());
}

TEST_CASE ("Browser loads into IR B when opened from slot B", "[gui][ir-browser]")
{
    BrowserFixture fixture;

    const auto ir = fixture.makeIrFile ("b-cab.wav");

    fixture.browseFromSlot ("irSlotB");
    fixture.panel->applyScanResults ({ ir }, fixture.libraryRoot.root);

    fixture.fileList->selectRow (0);

    CHECK (fixture.processor.getCurrentIrFilePathB() == ir.getFullPathName());
    CHECK (fixture.processor.getCurrentIrFilePath().isEmpty());

    // The slot's cartridge window reflects the browsed load, exactly as it
    // does for the direct file-chooser path (name shown without extension -
    // the smoked-glass window is a display, not a path field).
    auto* slotB = fixture.slotComponentFor ("irSlotB");
    REQUIRE (slotB != nullptr);
    CHECK (slotB->irName_forTest().contains ("b-cab"));
}

TEST_CASE ("The filter narrows the listing case-insensitively and the status text reports it", "[gui][ir-browser]")
{
    BrowserFixture fixture;

    const auto mesa = fixture.makeIrFile ("Mesa 4x12.wav");
    fixture.makeIrFile ("Ampeg 8x10.wav");

    fixture.browseFromSlot ("irSlotA");
    fixture.panel->applyScanResults ({ fixture.libraryRoot.root.getChildFile ("Ampeg 8x10.wav"), mesa },
                                     fixture.libraryRoot.root);
    REQUIRE (fixture.panel->getNumVisibleFiles() == 2);

    fixture.panel->setFilterText ("mesa");
    REQUIRE (fixture.panel->getNumVisibleFiles() == 1);
    CHECK (fixture.panel->displayNameForRow (0) == "Mesa 4x12.wav");

    auto* status = dynamic_cast<juce::Label*> (fixture.panel->findChildWithID ("irBrowser.status"));
    REQUIRE (status != nullptr);
    CHECK (status->getText().contains ("1 of 2"));
    // A11y: the status is mirrored into the accessible title (suite pattern:
    // visible text and AT surface never diverge).
    CHECK (status->getTitle() == status->getText());

    fixture.panel->setFilterText ("no such cab");
    CHECK (fixture.panel->getNumVisibleFiles() == 0);
    CHECK (status->getText().contains ("No matches"));

    fixture.panel->setFilterText ("");
    CHECK (fixture.panel->getNumVisibleFiles() == 2);
}

TEST_CASE ("Escape dismisses the browser", "[gui][a11y][ir-browser]")
{
    BrowserFixture fixture;

    fixture.browseFromSlot ("irSlotA");
    REQUIRE (fixture.panel->isVisible());

    juce::Component& panelAsComponent = *fixture.panel;
    CHECK (panelAsComponent.keyPressed (juce::KeyPress (juce::KeyPress::escapeKey)));
    CHECK_FALSE (fixture.panel->isVisible());
}

TEST_CASE ("Choosing a library folder persists it to the plugin state and it is reused on reopen", "[gui][ir-browser][state]")
{
    BrowserFixture fixture;

    fixture.browseFromSlot ("irSlotA");

    // Drive the folder change through the same public path the (native,
    // async) directory chooser callback uses.
    fixture.panel->setLibraryDirectory (fixture.libraryRoot.root);

    CHECK (fixture.processor.apvts.state.getProperty (ParamIDs::irLibraryFolderProperty).toString()
           == fixture.libraryRoot.root.getFullPathName());

    // Reopening reads the stored folder back out of the state - visible via
    // the folder label the panel shows.
    fixture.panel->keyPressed (juce::KeyPress (juce::KeyPress::escapeKey));
    fixture.browseFromSlot ("irSlotB");

    auto* folderLabel = dynamic_cast<juce::Label*> (fixture.panel->findChildWithID ("irBrowser.folderLabel"));
    REQUIRE (folderLabel != nullptr);
    CHECK (folderLabel->getText() == fixture.libraryRoot.root.getFullPathName());
}

TEST_CASE ("Open browser overlay renders non-blank over the faceplate (snapshot for PR review)", "[gui][ir-browser]")
{
    BrowserFixture fixture;

    fixture.makeIrFile ("Mesa 4x12 SM57 cap edge.wav");
    fixture.makeIrFile ("Ampeg 8x10 R121 1m.wav");

    fixture.browseFromSlot ("irSlotA");
    fixture.panel->applyScanResults ({ fixture.libraryRoot.root.getChildFile ("Ampeg 8x10 R121 1m.wav"),
                                       fixture.libraryRoot.root.getChildFile ("Mesa 4x12 SM57 cap edge.wav") },
                                     fixture.libraryRoot.root);
    fixture.fileList->selectRow (0);

    // Same SoftwareImageType/non-blank technique as EditorSnapshotTests.cpp.
    const auto snapshot = fixture.editor->createComponentSnapshot (fixture.editor->getLocalBounds(),
                                                                   true, 1.0f, juce::SoftwareImageType {});
    REQUIRE (snapshot.isValid());

    const auto reference = snapshot.getPixelAt (0, 0);
    bool foundDifference = false;

    for (int y = 0; y < snapshot.getHeight() && ! foundDifference; y += juce::jmax (1, snapshot.getHeight() / 20))
        for (int x = 0; x < snapshot.getWidth() && ! foundDifference; x += juce::jmax (1, snapshot.getWidth() / 20))
            if (snapshot.getPixelAt (x, y) != reference)
                foundDifference = true;

    CHECK (foundDifference);

    // Written next to gui-preview.png (build/ir-browser-preview.png under
    // `ctest --test-dir build`) for local/PR review.
    juce::PNGImageFormat pngFormat;
    const auto outFile = juce::File::getCurrentWorkingDirectory().getChildFile ("ir-browser-preview.png");

    if (auto stream = std::unique_ptr<juce::FileOutputStream> (outFile.createOutputStream()))
    {
        stream->setPosition (0);
        stream->truncate();
        CHECK (pngFormat.writeImageToStream (snapshot, *stream));
    }
}

TEST_CASE ("Browser rows expose their display names to accessibility clients", "[gui][a11y][ir-browser]")
{
    BrowserFixture fixture;

    const auto ir = fixture.makeIrFile ("rooms/close.wav");

    fixture.browseFromSlot ("irSlotA");
    fixture.panel->applyScanResults ({ ir }, fixture.libraryRoot.root);

    // getNameForRow() (the ListBox accessibility name source, JUCE 8.0.14
    // juce_ListBox.cpp RowAccessibilityHandler) is fed by the same
    // displayNameForRow() the row painter uses - one string for both
    // surfaces, so they cannot drift.
    CHECK (fixture.panel->displayNameForRow (0)
           == juce::String ("rooms") + juce::File::getSeparatorString() + "close.wav");
}

//==============================================================================
// "Install Library" (issue #33): the browser's affordance for writing the
// plugin's bundled IR library into the folder it already scans by default.
//
// These use a standalone IrBrowserPanel rather than the editor's, deliberately.
// The editor decides whether to offer the install by looking at the REAL
// default library folder (~/Music/Nave/Impulse Responses), so a test driven
// through the editor would pass or fail depending on whether the machine
// running it happens to have the library installed - and clicking the button
// there would write into the developer's own Music folder. The install itself
// is covered against temporary directories in tests/FactoryIrInstallTests.cpp.

TEST_CASE ("Install Library is offered only when the owner both wires it and asks for it",
           "[gui][ir-browser][install]")
{
    basilica::gui::IrBrowserPanel panel;

    auto* installButton = dynamic_cast<juce::TextButton*> (panel.findChildWithID ("irBrowser.installButton"));
    REQUIRE (installButton != nullptr);

    // Hidden by default: a sibling plugin reusing this panel may bundle
    // nothing at all, and must not inherit a button for it.
    CHECK_FALSE (panel.isFactoryLibraryInstallOffered());

    // Asked for, but with no handler wired - offering a button that could only
    // do nothing would be worse than not offering one.
    panel.setFactoryLibraryInstallOffered (true);
    CHECK_FALSE (panel.isFactoryLibraryInstallOffered());

    int installRequests = 0;
    panel.onInstallFactoryLibrary = [&installRequests] { ++installRequests; };

    panel.setFactoryLibraryInstallOffered (true);
    CHECK (panel.isFactoryLibraryInstallOffered());

    REQUIRE (installButton->onClick);
    installButton->onClick();
    CHECK (installRequests == 1);

    // The owner withdraws the offer once the library is installed.
    panel.setFactoryLibraryInstallOffered (false);
    CHECK_FALSE (panel.isFactoryLibraryInstallOffered());
}

TEST_CASE ("An empty library folder points at the install when one is on offer",
           "[gui][ir-browser][install]")
{
    basilica::gui::IrBrowserPanel panel;
    panel.onInstallFactoryLibrary = [] {};

    auto* status = dynamic_cast<juce::Label*> (panel.findChildWithID ("irBrowser.status"));
    REQUIRE (status != nullptr);

    const juce::File emptyRoot;

    panel.applyScanResults ({}, emptyRoot);
    CHECK (status->getText() == juce::String ("No impulse responses found - choose a library folder"));

    panel.setFactoryLibraryInstallOffered (true);
    panel.applyScanResults ({}, emptyRoot);

    // The one moment the hint is actually actionable: nothing to list, and a
    // library sitting inside the binary waiting to be written out.
    CHECK (status->getText().contains ("install the bundled library"));
}

TEST_CASE ("A failed install is reported in the status row and cleared by the next scan",
           "[gui][ir-browser][install]")
{
    BrowserFixture fixture;

    auto* status = dynamic_cast<juce::Label*> (fixture.panel->findChildWithID ("irBrowser.status"));
    REQUIRE (status != nullptr);

    // A successful install reports itself by the cabinets appearing in the
    // list. A failed one cannot - the folder looks exactly as empty as it did
    // before - so it is the case that needs a message.
    fixture.panel->showStatusMessage ("Could not install 1 file: modelled_4x12_ceramic_cone.wav");
    CHECK (status->getText().contains ("Could not install"));

    // Any subsequent scan re-derives the row from the listing, so a stale
    // failure cannot outlive the folder state it described.
    fixture.panel->setLibraryDirectory (fixture.libraryRoot.root);
    CHECK_FALSE (status->getText().contains ("Could not install"));
}

TEST_CASE ("A cabinet from a freshly installed library loads into the target slot",
           "[gui][ir-browser][install][processor]")
{
    BrowserFixture fixture;

    // End-to-end over the wiring this issue adds: embedded bytes -> installed
    // files -> the browser's own directory scan -> the slot load. The install
    // targets the fixture's temporary root, never the real library folder.
    const auto install = basilica::ir::FactoryIrLibrary::installInto (fixture.libraryRoot.root,
                                                                      nave::factoryIrAssets());
    INFO ("install summary: " << install.summary());
    REQUIRE (install.succeeded());

    const auto scanned = basilica::ir::IrLibrary::scan (fixture.libraryRoot.root);
    REQUIRE (scanned.size() == 9);

    fixture.browseFromSlot ("irSlotA");
    fixture.panel->applyScanResults (scanned, fixture.libraryRoot.root);

    REQUIRE (fixture.panel->getNumVisibleFiles() == 9);

    // Every row is a model, and reads as one in the list the user sees.
    for (int row = 0; row < fixture.panel->getNumVisibleFiles(); ++row)
        CHECK (fixture.panel->displayNameForRow (row).startsWith ("modelled_"));

    fixture.fileList->selectRow (0);
    CHECK (fixture.processor.getCurrentIrFilePath() == scanned.getFirst().getFullPathName());
}
