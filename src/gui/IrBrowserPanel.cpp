#include "IrBrowserPanel.h"

#include "BasilicaLookAndFeel.h"

namespace basilica::gui
{
    namespace
    {
        // Overlay geometry (@1x-equivalent; the panel lays out in whatever
        // bounds the editor gives it, so these are proportional caps rather
        // than plate-anchored coordinates - deliberately NOT part of
        // nave::layout, which is reserved for faceplate-art geometry the
        // layout-manifest tests pin).
        constexpr int panelMaxWidth = 620;
        constexpr int panelMargin = 40;
        constexpr int panelPadding = 16;
        constexpr int headerRowHeight = 28;
        constexpr int controlRowHeight = 28;
        constexpr int statusRowHeight = 24;
        constexpr int rowGap = 8;
        constexpr int listRowHeight = 22;
        constexpr float panelCornerSize = 8.0f;

        // Scrim + panel colours, derived from the suite palette: the scrim
        // dims the faceplate enough that the overlay reads as modal, the
        // panel body reuses the label backing chip's warm near-black so the
        // gold text/border pair keeps its WCAG-verified contrast (see
        // BasilicaLookAndFeel.cpp's A-03 notes).
        const juce::Colour scrimColour { 0xa6000000 };
        const juce::Colour selectedRowFill { 0xffffd24c };  // focusRingGold - selection reads as "focused"
        const juce::Colour selectedRowText { 0xff17110c };  // backing-chip dark on gold: high contrast
    }

    IrBrowserPanel::IrBrowserPanel()
    {
        setComponentID ("irBrowser");
        setWantsKeyboardFocus (true);

        // The overlay behaves as its own focus scope while visible, so Tab
        // cycles through the browser's controls instead of escaping into
        // the (visually dimmed) faceplate behind it.
        setFocusContainerType (juce::Component::FocusContainerType::focusContainer);

        titleLabel.setComponentID ("irBrowser.title");
        titleLabel.setJustificationType (juce::Justification::centredLeft);
        titleLabel.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (titleLabel);

        folderLabel.setComponentID ("irBrowser.folderLabel");
        folderLabel.setJustificationType (juce::Justification::centredLeft);
        folderLabel.setMinimumHorizontalScale (0.7f);
        addAndMakeVisible (folderLabel);

        folderButton.setComponentID ("irBrowser.folderButton");
        folderButton.setButtonText ("Folder...");
        folderButton.setTitle ("Choose impulse response library folder");
        folderButton.onClick = [this] { chooseFolder(); };
        addAndMakeVisible (folderButton);

        filterEditor.setComponentID ("irBrowser.filter");
        filterEditor.setTitle ("Filter impulse responses");
        filterEditor.setTextToShowWhenEmpty ("Filter...", juce::Colours::grey);
        filterEditor.setSelectAllWhenFocused (true);
        filterEditor.onTextChange = [this] { setFilterText (filterEditor.getText()); };
        filterEditor.onEscapeKey = [this] { dismiss(); };
        // Return in the filter box hands focus to the list so Up/Down/Return
        // work immediately - grabKeyboardFocus() needs a live native peer,
        // which a headless test binary doesn't have, hence the isShowing()
        // guard (same guard as open() below).
        filterEditor.onReturnKey = [this]
        {
            if (isShowing())
                fileList.grabKeyboardFocus();
        };
        addAndMakeVisible (filterEditor);

        fileList.setComponentID ("irBrowser.list");
        fileList.setTitle ("Impulse responses");
        fileList.setModel (this);
        fileList.setRowHeight (listRowHeight);
        fileList.setColour (juce::ListBox::backgroundColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (fileList);

        statusLabel.setComponentID ("irBrowser.status");
        statusLabel.setJustificationType (juce::Justification::centredLeft);
        statusLabel.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (statusLabel);

        closeButton.setComponentID ("irBrowser.closeButton");
        closeButton.setButtonText ("Close");
        closeButton.setTitle ("Close impulse response browser");
        closeButton.onClick = [this] { dismiss(); };
        addAndMakeVisible (closeButton);

        // Async scan delivery: hop scanner thread -> message thread with a
        // SafePointer (the panel may be destroyed while the message is in
        // flight - e.g. the host tearing the editor down mid-scan), then
        // re-check the generation stamp AFTER the hop, in case yet another
        // scan was requested while this delivery was queued.
        scanner.onScanFinished = [safeThis = juce::Component::SafePointer<IrBrowserPanel> (this)]
                                 (juce::Array<juce::File> scanned, juce::File scannedRoot, int generation)
        {
            juce::MessageManager::callAsync ([safeThis, files = std::move (scanned), scannedRoot, generation]() mutable
            {
                if (safeThis == nullptr)
                    return;

                if (generation != safeThis->scanner.latestGeneration())
                    return; // superseded while queued - a fresher delivery is coming

                safeThis->applyScanResults (std::move (files), scannedRoot);
            });
        };

        updateStatusText();
    }

    IrBrowserPanel::~IrBrowserPanel() = default;

    void IrBrowserPanel::open (const juce::String& targetSlotName, const juce::File& libraryFolder)
    {
        const auto titleText = "IR Browser - " + targetSlotName;
        titleLabel.setText (titleText, juce::dontSendNotification);
        titleLabel.setTitle (titleText);
        setTitle (titleText);

        currentRoot = libraryFolder.getFullPathName().isNotEmpty() ? libraryFolder
                                                                   : basilica::ir::IrLibrary::defaultDirectory();

        setVisible (true);
        toFront (true);

        // Always rescan on open: the library folder's contents may have
        // changed since the last visit, and a scan of an unchanged folder
        // is cheap.
        beginScan();

        if (isShowing())
            fileList.grabKeyboardFocus();
    }

    void IrBrowserPanel::setLibraryDirectory (const juce::File& newFolder)
    {
        currentRoot = newFolder;

        if (onLibraryFolderChanged != nullptr)
            onLibraryFolderChanged (newFolder);

        beginScan();
    }

    void IrBrowserPanel::beginScan()
    {
        scanning = true;
        allFiles.clearQuick();
        lastLoadedRow = -1;
        rebuildVisibleRows();

        folderLabel.setText (currentRoot.getFullPathName(), juce::dontSendNotification);
        folderLabel.setTitle ("Library folder: " + currentRoot.getFullPathName());

        scanner.startScan (currentRoot);
        updateStatusText();
    }

    void IrBrowserPanel::applyScanResults (juce::Array<juce::File> scannedFiles, const juce::File& scannedRoot)
    {
        scanning = false;
        currentRoot = scannedRoot;
        allFiles = std::move (scannedFiles);
        lastLoadedRow = -1;

        // Normally a no-op (beginScan() already showed this root), but keeps
        // the label honest if results ever arrive for a root the label has
        // moved past - and for tests driving this seam directly.
        folderLabel.setText (currentRoot.getFullPathName(), juce::dontSendNotification);
        folderLabel.setTitle ("Library folder: " + currentRoot.getFullPathName());

        rebuildVisibleRows();
        updateStatusText();
    }

    void IrBrowserPanel::setFilterText (const juce::String& newFilterText)
    {
        const auto normalised = newFilterText.trim().toLowerCase();

        if (normalised == filterText)
            return;

        filterText = normalised;

        // Keep the visible editor in sync when this is driven directly
        // (tests, or a future programmatic clear) rather than via its own
        // onTextChange - dontSendNotification avoids a feedback loop.
        if (filterEditor.getText().trim().toLowerCase() != filterText)
            filterEditor.setText (newFilterText, juce::dontSendNotification);

        lastLoadedRow = -1;
        rebuildVisibleRows();
        updateStatusText();
    }

    void IrBrowserPanel::rebuildVisibleRows()
    {
        visibleRows.clearQuick();

        for (int i = 0; i < allFiles.size(); ++i)
        {
            const auto relativeName = allFiles.getReference (i).getRelativePathFrom (currentRoot);

            if (filterText.isEmpty() || relativeName.toLowerCase().contains (filterText))
                visibleRows.add (i);
        }

        fileList.deselectAllRows();
        fileList.updateContent();
        fileList.repaint();
    }

    void IrBrowserPanel::updateStatusText()
    {
        juce::String text;

        if (scanning)
            text = "Scanning...";
        else if (allFiles.isEmpty())
            text = "No impulse responses found - choose a library folder";
        else if (visibleRows.isEmpty())
            text = "No matches for the current filter";
        else if (filterText.isNotEmpty())
            text = juce::String (visibleRows.size()) + " of " + juce::String (allFiles.size()) + " impulse responses";
        else
            text = juce::String (allFiles.size()) + " impulse responses";

        statusLabel.setText (text, juce::dontSendNotification);
        statusLabel.setTitle (text);
    }

    void IrBrowserPanel::chooseFolder()
    {
        activeFolderChooser = std::make_unique<juce::FileChooser> ("Choose your impulse response library folder...",
                                                                   currentRoot.isDirectory() ? currentRoot : juce::File());

        constexpr auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories;

        activeFolderChooser->launchAsync (flags, [this] (const juce::FileChooser& chooser)
        {
            const auto folder = chooser.getResult();

            if (folder.isDirectory())
                setLibraryDirectory (folder);
        });
    }

    juce::String IrBrowserPanel::displayNameForRow (int rowNumber) const
    {
        if (! juce::isPositiveAndBelow (rowNumber, visibleRows.size()))
            return {};

        const auto& file = allFiles.getReference (visibleRows[rowNumber]);
        const auto relative = file.getRelativePathFrom (currentRoot);

        // getRelativePathFrom() falls back to ".."-prefixed (or absolute)
        // paths for files outside the root - shouldn't happen for scan
        // results, but a bare file name is the more readable fallback.
        return relative.startsWith ("..") ? file.getFileName() : relative;
    }

    //==========================================================================
    // ListBoxModel

    int IrBrowserPanel::getNumRows() { return visibleRows.size(); }

    juce::String IrBrowserPanel::getNameForRow (int rowNumber) { return displayNameForRow (rowNumber); }

    void IrBrowserPanel::paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height, bool rowIsSelected)
    {
        if (rowIsSelected)
        {
            g.setColour (selectedRowFill);
            g.fillRect (0, 0, width, height);
            g.setColour (selectedRowText);
        }
        else
        {
            g.setColour (BasilicaLookAndFeel::getLabelTextColour());
        }

        g.setFont (juce::Font (juce::FontOptions {}.withHeight (14.0f)));
        g.drawText (displayNameForRow (rowNumber),
                    8, 0, width - 16, height, juce::Justification::centredLeft, true);
    }

    void IrBrowserPanel::selectedRowsChanged (int lastRowSelected)
    {
        // Audition-on-select: stepping through rows (arrow keys or single
        // clicks) loads immediately. -1 arrives from deselectAllRows() in
        // rebuildVisibleRows(); the lastLoadedRow guard keeps a re-selection
        // of the already-loaded row from redundantly reloading it.
        if (lastRowSelected < 0 || lastRowSelected == lastLoadedRow)
            return;

        loadRow (lastRowSelected, false);
    }

    void IrBrowserPanel::returnKeyPressed (int rowNumber) { loadRow (rowNumber, true); }

    void IrBrowserPanel::listBoxItemDoubleClicked (int rowNumber, const juce::MouseEvent&) { loadRow (rowNumber, true); }

    void IrBrowserPanel::loadRow (int rowNumber, bool dismissAfterLoad)
    {
        if (! juce::isPositiveAndBelow (rowNumber, visibleRows.size()))
            return;

        if (rowNumber != lastLoadedRow)
        {
            const auto& file = allFiles.getReference (visibleRows[rowNumber]);

            if (onIrChosen != nullptr)
                onIrChosen (file);

            lastLoadedRow = rowNumber;
        }

        if (dismissAfterLoad)
            dismiss();
    }

    void IrBrowserPanel::dismiss()
    {
        if (onDismiss != nullptr)
            onDismiss();
        else
            setVisible (false);
    }

    //==========================================================================
    // Component

    juce::Rectangle<int> IrBrowserPanel::panelBounds() const
    {
        auto bounds = getLocalBounds().reduced (panelMargin);
        bounds = bounds.withSizeKeepingCentre (juce::jmin (panelMaxWidth, bounds.getWidth()), bounds.getHeight());
        return bounds;
    }

    void IrBrowserPanel::paint (juce::Graphics& g)
    {
        g.fillAll (scrimColour);

        const auto panel = panelBounds().toFloat();

        g.setColour (BasilicaLookAndFeel::getLabelBackingChipColour());
        g.fillRoundedRectangle (panel, panelCornerSize);

        g.setColour (BasilicaLookAndFeel::getLabelTextColour());
        g.drawRoundedRectangle (panel.reduced (0.75f), panelCornerSize, 1.5f);
    }

    void IrBrowserPanel::paintOverChildren (juce::Graphics& g)
    {
        // WCAG 2.4.7 focus visibility for the list: unlike the panel's
        // TextButtons (LookAndFeel_V4 already restyles those on focus) and
        // the TextEditor (focusedOutlineColourId), a ListBox gets no default
        // focus treatment - reuse the suite's shared ring instead.
        if (fileList.hasKeyboardFocus (true))
            paintFocusRing (g, fileList.getBounds().toFloat().expanded (2.0f), FocusRingShape::roundedRectangle);
    }

    void IrBrowserPanel::focusOfChildComponentChanged (FocusChangeType)
    {
        repaint(); // keep the list's focus ring in sync
    }

    void IrBrowserPanel::resized()
    {
        auto content = panelBounds().reduced (panelPadding);

        auto headerRow = content.removeFromTop (headerRowHeight);
        closeButton.setBounds (headerRow.removeFromRight (72));
        headerRow.removeFromRight (rowGap);
        titleLabel.setBounds (headerRow);

        content.removeFromTop (rowGap);

        auto folderRow = content.removeFromTop (controlRowHeight);
        folderButton.setBounds (folderRow.removeFromRight (88));
        folderRow.removeFromRight (rowGap);
        folderLabel.setBounds (folderRow);

        content.removeFromTop (rowGap);

        filterEditor.setBounds (content.removeFromTop (controlRowHeight));

        content.removeFromTop (rowGap);

        statusLabel.setBounds (content.removeFromBottom (statusRowHeight));
        content.removeFromBottom (rowGap);

        fileList.setBounds (content);
    }

    bool IrBrowserPanel::keyPressed (const juce::KeyPress& key)
    {
        if (key == juce::KeyPress::escapeKey)
        {
            dismiss();
            return true;
        }

        return false;
    }

    void IrBrowserPanel::mouseDown (const juce::MouseEvent& event)
    {
        // Clicking the dimmed scrim outside the panel closes the browser -
        // the standard modal-overlay affordance. Clicks INSIDE the panel
        // land on child components and never reach this handler unless they
        // hit panel padding, which panelBounds().contains() keeps open.
        if (! panelBounds().contains (event.getPosition()))
            dismiss();
    }
}
