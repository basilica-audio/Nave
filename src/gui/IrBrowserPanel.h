#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../ir/IrLibrary.h"

namespace basilica::gui
{
    // The IR browser (issue #1): a full-editor overlay panel that lists every
    // WAV/AIFF impulse response under a user-chosen library folder and loads
    // the selection into whichever IR slot (A or B) it was opened for.
    // Complements - rather than replaces - each slot's direct "Load IR..."
    // file chooser: the chooser is for grabbing one known file, the browser
    // is for *auditioning* across a library.
    //
    // Interaction model (documented in docs/manual.md):
    //   * Selecting a row (mouse click, or Up/Down once the list has focus)
    //     loads that IR into the target slot IMMEDIATELY - the browser is an
    //     audition surface, so stepping through rows steps through cabs.
    //   * Return or double-click loads the row and closes the browser.
    //   * Escape closes it (from the list, the filter box, or the panel).
    //   * The filter box narrows the listing by substring, case-insensitive.
    //   * "Folder..." picks a different library root; the choice is persisted
    //     via onLibraryFolderChanged (a plain apvts.state property, like the
    //     IR file paths themselves - see ParamIDs::irLibraryFolderProperty).
    //
    // Directory scanning runs on IrLibraryScanner's background thread (see
    // IrLibrary.h) and is delivered back here via Component::SafePointer +
    // MessageManager::callAsync, with a generation stamp so a stale scan
    // never overwrites a newer one. Nothing in this component touches the
    // audio thread; the loads it triggers go through the processor's
    // documented message-thread IR-loading path.
    //
    // Styling follows BasilicaLookAndFeel: the suite's gold-on-dark palette
    // (the panel reuses getLabelTextColour()/getLabelBackingChipColour() so
    // it can never drift from the label styling), standard TextButtons
    // (which LookAndFeel_V4 already gives a visible keyboard-focus
    // treatment), and paintFocusRing() around the list when it holds
    // keyboard focus - the same WCAG 2.4.7 pattern FilmstripKnob uses.
    class IrBrowserPanel final : public juce::Component,
                                 private juce::ListBoxModel
    {
    public:
        IrBrowserPanel();
        ~IrBrowserPanel() override;

        // Fired (message thread) when the user picks a row. The owner
        // performs the actual slot load - the panel stays slot-agnostic so
        // sibling plugins can reuse it unchanged.
        std::function<void (const juce::File& irFile)> onIrChosen;

        // Fired when the user asks to close the panel (Escape, the Close
        // button, clicking the scrim, or a load-and-close gesture). The
        // OWNER hides the panel (and restores focus); the panel never hides
        // itself, so visibility stays under one component's control.
        std::function<void()> onDismiss;

        // Fired when the user picks a different library folder, so the
        // owner can persist it into the plugin state.
        std::function<void (const juce::File& newFolder)> onLibraryFolderChanged;

        // Shows the panel targeting the given slot ("IR A"/"IR B") and
        // (re)scans `libraryFolder` (falling back to
        // IrLibrary::defaultDirectory() if it is not a valid path).
        void open (const juce::String& targetSlotName, const juce::File& libraryFolder);

        // Switches the library root, notifies onLibraryFolderChanged, and
        // rescans. Public so tests can drive the folder flow without the
        // (native, async) directory chooser.
        void setLibraryDirectory (const juce::File& newFolder);

        // Applies the substring filter. Public seam for tests: the filter
        // TextEditor's change notifications are posted asynchronously (JUCE
        // 8.0.14 TextEditor::textChanged() uses postCommandMessage), which
        // never fires in a headless, no-message-loop test binary - tests
        // call this directly, the editor's onTextChange calls it too.
        void setFilterText (const juce::String& newFilterText);

        // Replaces the current listing. This is both the async scan
        // delivery target and the deterministic test seam (the scanner's
        // own async path is covered separately in tests/IrLibraryTests.cpp).
        void applyScanResults (juce::Array<juce::File> scannedFiles, const juce::File& scannedRoot);

        // The visible (post-filter) row's display name - the path relative
        // to the library root. Also what getNameForRow() reports to
        // accessibility clients.
        juce::String displayNameForRow (int rowNumber) const;

        int getNumVisibleFiles() const noexcept { return visibleRows.size(); }

        void paint (juce::Graphics& g) override;
        void paintOverChildren (juce::Graphics& g) override;
        void resized() override;
        bool keyPressed (const juce::KeyPress& key) override;
        void mouseDown (const juce::MouseEvent& event) override;
        void focusOfChildComponentChanged (FocusChangeType cause) override;

    private:
        // ListBoxModel
        int getNumRows() override;
        void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected) override;
        void listBoxItemDoubleClicked (int rowNumber, const juce::MouseEvent& event) override;
        void returnKeyPressed (int rowNumber) override;
        void selectedRowsChanged (int lastRowSelected) override;
        juce::String getNameForRow (int rowNumber) override;

        void beginScan();
        void rebuildVisibleRows();
        void updateStatusText();
        void chooseFolder();
        void loadRow (int rowNumber, bool dismissAfterLoad);
        void dismiss();
        juce::Rectangle<int> panelBounds() const;

        juce::Label titleLabel;
        juce::Label folderLabel;
        juce::Label statusLabel;
        juce::TextButton folderButton;
        juce::TextButton closeButton;
        juce::TextEditor filterEditor;
        juce::ListBox fileList;

        std::unique_ptr<juce::FileChooser> activeFolderChooser;

        basilica::ir::IrLibraryScanner scanner;
        juce::File currentRoot;
        juce::Array<juce::File> allFiles;
        juce::Array<int> visibleRows; // indices into allFiles, post-filter
        juce::String filterText;      // lower-cased, trimmed
        int lastLoadedRow = -1;       // index into visibleRows
        bool scanning = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IrBrowserPanel)
    };
}
