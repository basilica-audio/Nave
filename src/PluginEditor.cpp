#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"
#include "gui/ImageDensity.h"
#include "ir/FactoryIrLibrary.h"
#include "ir/IrLibrary.h"
#include "params/ParameterIds.h"
#include "presets/Localisation.h"

#include <BinaryData.h>

namespace
{
    // Base (@1x, 100% scale) faceplate geometry lives in PluginEditorLayout.h
    // (nave::layout) rather than here, so tests/gui/EditorLayoutTests.cpp can
    // assert layout invariants against the exact constants this file lays
    // components out with - see that header's docs.
    using namespace nave::layout;


    // Nave's 6 parameters split across the tone/character/output bays
    // (.scaffold/gui-assets/faceplate-nave-v1/layout-manifest.json), 2 knobs
    // per bay, in the same signal-flow order as the faceplate design brief:
    // character (what shapes the cab) -> tone (post-convolution HP/LP) ->
    // output (dry/wet + trim) reads left-to-right in the manifest as
    // tone/character/output, so the knobLayout table below follows that
    // left-to-right bay order rather than a separate signal-flow ordering.
    enum class Bay
    {
        tone,
        character,
        output
    };

    struct KnobLayoutEntry
    {
        const char* parameterId;
        const char* labelText;
        Bay bay;
        int col; // 0 = left knob in the bay, 1 = right knob
    };

    constexpr std::array<KnobLayoutEntry, 6> knobLayout {
        KnobLayoutEntry { ParamIDs::loCut, "LoCut", Bay::tone, 0 },
        KnobLayoutEntry { ParamIDs::hiCut, "HiCut", Bay::tone, 1 },
        KnobLayoutEntry { ParamIDs::irBlend, "IR Blend", Bay::character, 0 },
        KnobLayoutEntry { ParamIDs::micDistance, "Distance", Bay::character, 1 },
        KnobLayoutEntry { ParamIDs::mix, "Mix", Bay::output, 0 },
        KnobLayoutEntry { ParamIDs::level, "Level", Bay::output, 1 },
    };

    const juce::Rectangle<int>& bayRectFor (Bay bay)
    {
        switch (bay)
        {
            case Bay::tone: return toneBay1x;
            case Bay::character: return characterBay1x;
            case Bay::output: return outputBay1x;
        }

        return toneBay1x;
    }

    juce::Image loadImage (const char* data, int size)
    {
        return juce::ImageCache::getFromMemory (data, size);
    }

    // M2 i18n frame (.scaffold/specs/preset-system-m2.md): selects German
    // (resources/i18n/de.txt) or falls through to English, once, at editor
    // construction - see Localisation.h's docs. `presetBar` is a member
    // initialised via the constructor's initialiser list, and its own
    // constructor already calls TRANS() on every button label - member
    // initialisers run in declaration order regardless of the order
    // they're written in, so this helper (called from presetBar's own
    // initialiser expression below) is what actually guarantees
    // installLocalisation() runs before presetBar exists, not an
    // installLocalisation() call in the constructor *body*, which would run
    // too late. Copied from silentium's M3 pilot (src/PluginEditor.cpp).
    basilica::presets::PresetManager& initLocalisationThenGetPresetManager (NaveAudioProcessor& processor)
    {
        basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize);
        return processor.presetManager;
    }

    // Non-parameter, per-session UI state: the stepped scale choice (0/1/2)
    // stored as a plain property directly on apvts.state, exactly like
    // ParamIDs::irFilePathProperty (see that header's docs) and silentium's
    // own uiScaleStepProperty - round-trips through
    // getStateInformation()/setStateInformation() without needing a host-
    // automatable parameter for a view choice.
    constexpr const char* uiScaleStepProperty = "uiScaleStep";
}

// The bundled factory IR library (issue #33), as embedded bytes. Declared in
// PluginEditor.h; defined here because this is one of the two translation
// units allowed to include BinaryData.h (see CMakeLists.txt).
//
// This function is the whole of Nave's coupling to src/ir/FactoryIrLibrary.
// {h,cpp}: that module never sees BinaryData.h, exactly as src/presets/
// PresetManager.h never does, so both stay copyable into a sibling plugin
// that bundles different content.
//
// The nine .wav files are the audio; LICENSES.md, CC0-1.0.txt and
// manifest.json travel with them because #33's licensing bar is a licence
// file committed *alongside* the audio - an installed copy that left the
// provenance behind in the repository would not meet it. None of the three
// is an audio file, so IrLibrary::scan() never lists them as cabinets.
const std::vector<basilica::ir::FactoryIrAsset>& nave::factoryIrAssets()
{
    static const std::vector<basilica::ir::FactoryIrAsset> assets
    {
        // Guitar. The fourth field is the cabinet's stable id (issue #33),
        // the same string resources/irs/manifest.json records for that file -
        // identity, never a resolution key. See FactoryIrAsset::stableId and
        // docs/bundled-ir-library.md.
        { "modelled_4x12_ceramic_cone.wav", BinaryData::modelled_4x12_ceramic_cone_wav, BinaryData::modelled_4x12_ceramic_cone_wavSize, "guitar-412-cone" },
        { "modelled_4x12_ceramic_edge.wav", BinaryData::modelled_4x12_ceramic_edge_wav, BinaryData::modelled_4x12_ceramic_edge_wavSize, "guitar-412-edge" },
        { "modelled_4x12_ceramic_room.wav", BinaryData::modelled_4x12_ceramic_room_wav, BinaryData::modelled_4x12_ceramic_room_wavSize, "guitar-412-room" },
        { "modelled_2x12_alnico_cone.wav",  BinaryData::modelled_2x12_alnico_cone_wav,  BinaryData::modelled_2x12_alnico_cone_wavSize,  "guitar-212-alnico" },
        { "modelled_1x12_combo_cone.wav",   BinaryData::modelled_1x12_combo_cone_wav,   BinaryData::modelled_1x12_combo_cone_wavSize,   "guitar-112-combo" },

        // Bass. Byte-identical to the four Crypta bundles, generated from the
        // same script with the same model ids - so the ids match there too.
        { "modelled_8x10_cone.wav",         BinaryData::modelled_8x10_cone_wav,         BinaryData::modelled_8x10_cone_wavSize,         "bass-810-cone" },
        { "modelled_8x10_edge.wav",         BinaryData::modelled_8x10_edge_wav,         BinaryData::modelled_8x10_edge_wavSize,         "bass-810-edge" },
        { "modelled_1x15_vintage.wav",      BinaryData::modelled_1x15_vintage_wav,      BinaryData::modelled_1x15_vintage_wavSize,      "bass-115-vintage" },
        { "modelled_4x10_horn.wav",         BinaryData::modelled_4x10_horn_wav,         BinaryData::modelled_4x10_horn_wavSize,         "bass-410-horn" },

        // Provenance
        { "LICENSES.md",                    BinaryData::LICENSES_md,                    BinaryData::LICENSES_mdSize },
        // BinaryData::CC01_0_txt, not CC0_1_0_txt: juce_add_binary_data drops
        // the hyphen rather than mapping it to an underscore, so the symbol
        // for "CC0-1.0.txt" is not the mechanical substitution it looks like.
        { "CC0-1.0.txt",                    BinaryData::CC01_0_txt,                     BinaryData::CC01_0_txtSize },
        { "manifest.json",                  BinaryData::manifest_json,                  BinaryData::manifest_jsonSize },
    };

    return assets;
}

NaveAudioProcessorEditor::NaveAudioProcessorEditor (NaveAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      audioProcessor (processorToEdit),
      presetBar (initLocalisationThenGetPresetManager (processorToEdit))
{
    setLookAndFeel (&lookAndFeel);

    facePlateImage1x = loadImage (BinaryData::faceplate_nave_900x600_png, BinaryData::faceplate_nave_900x600_pngSize);
    facePlateImage2x = loadImage (BinaryData::faceplate_nave_1800x1200_png, BinaryData::faceplate_nave_1800x1200_pngSize);
    brandIconImage = loadImage (BinaryData::icon256_png, BinaryData::icon256_pngSize);

    // Creation order below doubles as the accessibility/keyboard focus order
    // (JUCE's default FocusTraverser walks children in z-order, i.e.
    // creation order, when no custom traverser is installed) - kept
    // deliberately matching the visual reading order: header/scale control,
    // preset bar, the IR A/IR B loader controls, then the knob bays
    // left-to-right (tone, character, output).
    titleLabel.setText ("Nave", juce::dontSendNotification);
    titleLabel.setJustificationType (juce::Justification::centredLeft);
    titleLabel.setFont (juce::Font (juce::FontOptions {}
                                        .withName (juce::Font::getDefaultSerifFontName())
                                        .withHeight (26.0f)
                                        .withStyle ("Bold")));
    titleLabel.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (titleLabel);

    addAndMakeVisible (presetBar);

    // A-05-equivalent (silentium M3 a11y review, applied here from the
    // start rather than as a follow-up fix): the accessible title is set
    // from applyScaleStep() below, which runs once here at construction and
    // again on every subsequent click, so it always reflects the CURRENT
    // scale rather than a static string. componentID lets tests find this
    // button without depending on its (dynamic) title.
    scaleButton.setComponentID ("scaleButton");
    scaleButton.onClick = [this] { cycleScale(); };
    addAndMakeVisible (scaleButton);

    const auto knobStrip1x = loadImage (BinaryData::knob_brass_strip_160px_128f_png, BinaryData::knob_brass_strip_160px_128f_pngSize);
    const auto knobStrip2x = loadImage (BinaryData::knob_brass_strip_320px_128f_png, BinaryData::knob_brass_strip_320px_128f_pngSize);

    for (size_t i = 0; i < knobLayout.size(); ++i)
    {
        auto& entry = knobLayout[i];
        knobs[i].slider = std::make_unique<basilica::gui::FilmstripKnob> (knobStrip1x, knobStrip2x, 128);
        configureKnob (knobs[i], entry.parameterId, entry.labelText);
    }

    configureIrSlot (irSlotA, IrSlotId::A, "IR A");
    configureIrSlot (irSlotB, IrSlotId::B, "IR B");

    // The IR browser overlay (issue #1): added LAST so it sits above every
    // other child when visible (z-order follows add order). Hidden until a
    // slot's Browse... button opens it; the editor owns visibility and the
    // slot targeting, the panel owns scanning/filtering/selection (see
    // IrBrowserPanel.h).
    irBrowserPanel.onIrChosen = [this] (const juce::File& irFile)
    {
        const auto loaded = irBrowserTargetSlot == IrSlotId::A
                                 ? audioProcessor.loadImpulseResponseFromFile (irFile)
                                 : audioProcessor.loadImpulseResponseFromFileB (irFile);

        if (loaded)
            refreshIrSlotLabel (slotFor (irBrowserTargetSlot), irBrowserTargetSlot, irBrowserTargetLabel);
    };
    irBrowserPanel.onLibraryFolderChanged = [this] (const juce::File& newFolder)
    {
        // Same plain-ValueTree-property persistence as the IR file paths
        // themselves (ParamIDs::irLibraryFolderProperty's docs).
        audioProcessor.apvts.state.setProperty (ParamIDs::irLibraryFolderProperty,
                                                newFolder.getFullPathName(), nullptr);

        // A preset's IR reference resolves against the folder the browser is
        // pointed at (issue #42), so the index has to follow the user's
        // choice rather than the one that was current at construction.
        audioProcessor.refreshIrSearchRoots();
    };
    // "Install Library" (issue #33): unpack the bundled IRs into the folder
    // the browser already scans by default, then point the browser at it.
    // Only reachable from the button, which openIrBrowserForSlot() only shows
    // when the library is not already installed intact - nothing writes to the
    // user's Music folder without an explicit click.
    irBrowserPanel.onInstallFactoryLibrary = [this]
    {
        const auto destination = basilica::ir::IrLibrary::defaultDirectory();
        const auto result = basilica::ir::FactoryIrLibrary::installInto (destination, nave::factoryIrAssets());

        if (! result.succeeded())
        {
            // The listing cannot express this: a failed install leaves the
            // folder looking exactly as empty as before it was attempted.
            irBrowserPanel.showStatusMessage (result.summary());
            return;
        }

        // setLibraryDirectory() fires onLibraryFolderChanged (persisting the
        // choice) and rescans, so the freshly written cabinets appear in the
        // list without any further gesture - which is the whole point of the
        // button, and is also what reports success: files, not a message.
        irBrowserPanel.setLibraryDirectory (destination);
        irBrowserPanel.setFactoryLibraryInstallOffered (false);

        // The bundled cabinets a factory preset may reference now exist, so a
        // preset that reported them missing a moment ago will resolve if it is
        // loaded again - and the notice about them is stale either way.
        audioProcessor.refreshIrSearchRoots();
        showPresetIrNotice ({});
    };
    irBrowserPanel.onDismiss = [this]
    {
        irBrowserPanel.setVisible (false);

        // Hand keyboard focus back to the Browse... button that opened the
        // overlay, so a keyboard/AT user lands where they left off.
        // grabKeyboardFocus() needs a live native peer - absent in headless
        // tests, hence the guard.
        auto& browseButton = slotFor (irBrowserTargetSlot).browseButton;
        if (browseButton.isShowing())
            browseButton.grabKeyboardFocus();
    };
    addChildComponent (irBrowserPanel);

    // Issue #42's non-modal notice. addChildComponent (not
    // addAndMakeVisible): it stays hidden, and takes up none of the plate,
    // until a preset actually reports a missing IR. Non-interactive, so it
    // is not part of the keyboard focus order and cannot swallow a click
    // meant for the art beneath it - but it does carry an accessible title,
    // so a screen-reader user is told the same thing a sighted one is.
    // componentID (the same convention scaleButton uses) lets a test find
    // the strip without depending on its text, which is translated.
    presetIrNoticeLabel.setComponentID ("presetIrNotice");
    presetIrNoticeLabel.setJustificationType (juce::Justification::centred);
    presetIrNoticeLabel.setMinimumHorizontalScale (1.0f);
    presetIrNoticeLabel.setInterceptsMouseClicks (false, false);
    presetIrNoticeLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    presetIrNoticeLabel.setColour (juce::Label::backgroundColourId, juce::Colours::black.withAlpha (0.72f));
    addChildComponent (presetIrNoticeLabel);

    audioProcessor.onPresetIrNotice = [this] (const juce::String& message)
    {
        // PresetManager is message-thread-only by contract (see its class
        // docs), and this callback only ever fires from inside a preset
        // load - so there is no thread hop to make here, and the editor
        // clears the callback in its destructor, so it cannot outlive us.
        showPresetIrNotice (message);
    };

    // A notice raised before this editor existed (a preset recalled by the
    // host on instantiation, or the startup default) would otherwise be
    // silently dropped.
    showPresetIrNotice (audioProcessor.getPresetIrNotice());

    setResizable (false, false);

    const auto storedStep = (int) audioProcessor.apvts.state.getProperty (uiScaleStepProperty, 0);
    applyScaleStep (juce::jlimit (0, (int) scaleSteps.size() - 1, storedStep));
}

NaveAudioProcessorEditor::~NaveAudioProcessorEditor()
{
    // Before anything else: the processor outlives the editor, and the
    // callback captures `this`.
    audioProcessor.onPresetIrNotice = nullptr;

    setLookAndFeel (nullptr);
}

void NaveAudioProcessorEditor::showPresetIrNotice (const juce::String& message)
{
    presetIrNoticeLabel.setText (message, juce::dontSendNotification);
    presetIrNoticeLabel.setTitle (message);
    presetIrNoticeLabel.setVisible (message.isNotEmpty());
}

void NaveAudioProcessorEditor::configureKnob (Knob& knob, const juce::String& parameterId, const juce::String& labelText)
{
    knob.slider->setPopupDisplayEnabled (true, true, this);
    knob.slider->setTitle (labelText);
    knob.slider->setName (labelText);
    addAndMakeVisible (*knob.slider);

    if (auto* param = audioProcessor.apvts.getParameter (parameterId))
    {
        const auto defaultValue = param->getNormalisableRange().convertFrom0to1 (param->getDefaultValue());
        knob.slider->setDoubleClickReturnValue (true, defaultValue);
    }

    knob.label.setText (labelText, juce::dontSendNotification);
    knob.label.setJustificationType (juce::Justification::centred);
    knob.label.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (knob.label);

    // SliderAttachment MUST be constructed before the textFromValueFunction
    // override below, not after: JUCE 8.0.14's SliderParameterAttachment
    // constructor (juce_ParameterAttachments.cpp:128) itself assigns
    // `slider.textFromValueFunction = [&param] (double v) { return
    // param.getText (...); }` (no unit) as part of wiring the attachment -
    // setting our own function BEFORE this point would be silently
    // clobbered the moment the attachment is created. See silentium's
    // src/PluginEditor.cpp (the M3 pilot) for the original writeup of this
    // ordering bug.
    knob.attachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, parameterId, *knob.slider);

    if (auto* param = audioProcessor.apvts.getParameter (parameterId))
    {
        // Every parameter declares its unit via .withLabel() in
        // ParameterLayout.cpp (Hz/%/dB), but SliderAttachment's own
        // textFromValueFunction (see above) formats the value but drops the
        // unit entirely. This feeds BOTH the popup value display
        // (setPopupDisplayEnabled above) and the accessibility value string
        // (juce_Slider.cpp's SliderAccessibilityHandler::ValueInterface::
        // getCurrentValueAsString() calls Slider::getTextFromValue(), which
        // calls this same function), so one fix here covers both surfaces.
        // Still uses the parameter's own getText() (not just a raw suffix)
        // so the reported precision/rounding matches what the host itself
        // would display.
        knob.slider->textFromValueFunction = [param] (double v)
        {
            return param->getText (param->convertTo0to1 ((float) v), 0) + " " + param->getLabel();
        };
        knob.slider->updateText();
    }
}

void NaveAudioProcessorEditor::configureIrSlot (IrSlot& slot, IrSlotId id, const juce::String& slotLabel)
{
    // componentIDs are set purely so tests/gui/EditorAccessibilityTests.cpp
    // can find these controls without depending on their (slot-specific,
    // human-readable) titles - same rationale as scaleButton's componentID.
    const auto idPrefix = id == IrSlotId::A ? juce::String ("irSlotA") : juce::String ("irSlotB");

    slot.nameLabel.setComponentID (idPrefix + ".nameLabel");
    slot.nameLabel.setJustificationType (juce::Justification::centredLeft);
    slot.nameLabel.setMinimumHorizontalScale (1.0f);
    addAndMakeVisible (slot.nameLabel);
    refreshIrSlotLabel (slot, id, slotLabel);

    slot.browseButton.setComponentID (idPrefix + ".browseButton");
    slot.browseButton.setButtonText ("Browse...");
    slot.browseButton.setTitle ("Browse impulse response library, " + slotLabel);
    slot.browseButton.onClick = [this, id, slotLabel] { openIrBrowserForSlot (id, slotLabel); };
    addAndMakeVisible (slot.browseButton);

    slot.loadButton.setComponentID (idPrefix + ".loadButton");
    slot.loadButton.setButtonText ("Load IR...");
    slot.loadButton.setTitle ("Load impulse response, " + slotLabel);
    slot.loadButton.onClick = [this, &slot, id, slotLabel] { chooseImpulseResponseForSlot (slot, id, slotLabel); };
    addAndMakeVisible (slot.loadButton);

    slot.defaultButton.setComponentID (idPrefix + ".defaultButton");
    slot.defaultButton.setButtonText ("Default");
    slot.defaultButton.setTitle ("Reset " + slotLabel + " to the default impulse response");
    slot.defaultButton.onClick = [this, &slot, id, slotLabel]
    {
        if (id == IrSlotId::A)
            audioProcessor.loadDefaultImpulseResponse();
        else
            audioProcessor.loadDefaultImpulseResponseB();

        refreshIrSlotLabel (slot, id, slotLabel);
    };
    addAndMakeVisible (slot.defaultButton);
}

void NaveAudioProcessorEditor::refreshIrSlotLabel (IrSlot& slot, IrSlotId id, const juce::String& slotLabel)
{
    const auto irPath = id == IrSlotId::A ? audioProcessor.getCurrentIrFilePath() : audioProcessor.getCurrentIrFilePathB();
    const auto displayText = slotLabel + ": " + (irPath.isEmpty() ? juce::String ("Default (no IR loaded)") : juce::File (irPath).getFileName());

    slot.nameLabel.setText (displayText, juce::dontSendNotification);
    slot.nameLabel.setTitle (displayText);

    // The user has just decided what is in this slot, which settles the
    // question any outstanding "this preset was made with..." notice was
    // asking (issue #42). Leaving it up would nag about a choice already made.
    showPresetIrNotice ({});
}

void NaveAudioProcessorEditor::chooseImpulseResponseForSlot (IrSlot& slot, IrSlotId id, const juce::String& slotLabel)
{
    const auto title = id == IrSlotId::A
                            ? "Load a cabinet impulse response..."
                            : "Load a secondary cabinet impulse response (IR B)...";

    slot.activeFileChooser = std::make_unique<juce::FileChooser> (title, juce::File(), "*.wav;*.aiff;*.aif");

    constexpr auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    slot.activeFileChooser->launchAsync (flags, [this, &slot, id, slotLabel] (const juce::FileChooser& chooser)
    {
        const auto file = chooser.getResult();

        if (! file.existsAsFile())
            return;

        const auto loaded = id == IrSlotId::A
                                 ? audioProcessor.loadImpulseResponseFromFile (file)
                                 : audioProcessor.loadImpulseResponseFromFileB (file);

        if (loaded)
            refreshIrSlotLabel (slot, id, slotLabel);
    });
}

NaveAudioProcessorEditor::IrSlot& NaveAudioProcessorEditor::slotFor (IrSlotId id) noexcept
{
    return id == IrSlotId::A ? irSlotA : irSlotB;
}

void NaveAudioProcessorEditor::openIrBrowserForSlot (IrSlotId id, const juce::String& slotLabel)
{
    irBrowserTargetSlot = id;
    irBrowserTargetLabel = slotLabel;

    const auto storedFolder = audioProcessor.apvts.state
                                  .getProperty (ParamIDs::irLibraryFolderProperty, juce::String())
                                  .toString();

    const auto libraryFolder = storedFolder.isNotEmpty() ? juce::File (storedFolder)
                                                         : basilica::ir::IrLibrary::defaultDirectory();

    // Offer the install only while there is something to install. Checked on
    // every open rather than once, because the default folder is an ordinary
    // folder the user can empty, move or partially delete between visits -
    // and checked byte-wise (FactoryIrLibrary::isInstalledIn) rather than by
    // existence, so a truncated file re-offers the install instead of leaving
    // a broken cabinet in the list. It is a stat per file in the common case.
    irBrowserPanel.setFactoryLibraryInstallOffered (
        ! basilica::ir::FactoryIrLibrary::isInstalledIn (basilica::ir::IrLibrary::defaultDirectory(),
                                                        nave::factoryIrAssets()));

    irBrowserPanel.open (slotLabel, libraryFolder);
}

void NaveAudioProcessorEditor::cycleScale()
{
    applyScaleStep ((scaleStepIndex + 1) % (int) scaleSteps.size());
}

void NaveAudioProcessorEditor::applyScaleStep (int newStepIndex)
{
    scaleStepIndex = juce::jlimit (0, (int) scaleSteps.size() - 1, newStepIndex);
    audioProcessor.apvts.state.setProperty (uiScaleStepProperty, scaleStepIndex, nullptr);

    const auto percentText = juce::String ((int) (scaleSteps[(size_t) scaleStepIndex] * 100.0f)) + "%";
    scaleButton.setButtonText (percentText);

    // An explicitly-set AccessibilityHandler title always wins over the
    // button's own text for screen readers (JUCE 8.0.14
    // juce_ButtonAccessibilityHandler.h), so a title set once at
    // construction and never updated would silently strand AT users on a
    // stale percentage forever. Re-setting the title here, alongside the
    // visible text, on every step change (construction included, since this
    // runs from the constructor too) keeps both surfaces in sync - see
    // silentium's src/PluginEditor.cpp (A-05 fix) for the original writeup.
    scaleButton.setTitle ("Window scale, " + percentText);

    const auto scale = scaleSteps[(size_t) scaleStepIndex];
    setSize ((int) std::lround ((float) baseEditorWidth * scale),
             (int) std::lround ((float) baseEditorHeight * scale));
}

void NaveAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    const auto scale = scaleSteps[(size_t) scaleStepIndex];
    const auto plateBounds = juce::Rectangle<float> (0.0f, (float) topStripHeight1x * scale + (float) topStripGap1x * scale,
                                                      (float) plateWidth1x * scale, (float) plateHeight1x * scale);

    const auto& plateImage = basilica::gui::pickImageForWidth (facePlateImage1x, facePlateImage2x,
                                                               plateWidth1x, (int) plateBounds.getWidth());
    if (plateImage.isValid())
        g.drawImage (plateImage, plateBounds);

    if (brandIconImage.isValid())
    {
        const auto d = (float) roundelRadius1x * 1.7f * scale;
        const auto cx = (float) roundelCentre1x.x * scale;
        const auto cy = plateBounds.getY() + (float) roundelCentre1x.y * scale;
        g.drawImage (brandIconImage, juce::Rectangle<float> (d, d).withCentre ({ cx, cy }));
    }
}

void NaveAudioProcessorEditor::resized()
{
    const auto scale = scaleSteps[(size_t) scaleStepIndex];
    const auto s = [scale] (int v) { return (int) std::lround ((float) v * scale); };

    auto bounds = getLocalBounds();
    auto topStrip = bounds.removeFromTop (s (topStripHeight1x));

    scaleButton.setBounds (topStrip.removeFromRight (s (scaleButtonWidth1x)));
    presetBar.setBounds (topStrip);

    // Everything below is expressed in plate-local coordinates (the base
    // @1x table in PluginEditorLayout.h), then offset by the top strip +
    // gap and scaled - same toPlateRect technique as silentium's editor.
    const auto toPlateRect = [&] (juce::Rectangle<int> plateLocal)
    {
        return juce::Rectangle<int> (s (plateLocal.getX()),
                                     s (topStripHeight1x + topStripGap1x) + s (plateLocal.getY()),
                                     s (plateLocal.getWidth()),
                                     s (plateLocal.getHeight()));
    };

    titleLabel.setBounds (toPlateRect (headerBay1x.withWidth (roundelCentre1x.x - headerBay1x.getX() - roundelRadius1x - 8)));

    const auto knobDiam = s (knobDiameter1x);
    const auto labelH = s (knobLabelHeight1x);

    for (size_t i = 0; i < knobLayout.size(); ++i)
    {
        auto& entry = knobLayout[i];
        const auto bay = toPlateRect (bayRectFor (entry.bay));
        const auto cellW = bay.getWidth() / knobBayCols;
        const auto cellX = bay.getX() + entry.col * cellW;

        knobs[i].label.setBounds (cellX, bay.getY(), cellW, labelH);
        knobs[i].slider->setBounds (juce::Rectangle<int> (knobDiam, knobDiam)
                                        .withCentre ({ cellX + cellW / 2, bay.getY() + labelH + (bay.getHeight() - labelH) / 2 }));
    }

    const auto irBay = toPlateRect (irLoaderBay1x);
    const auto halfW = irBay.getWidth() / 2;
    const auto innerMargin = s (irSlotInnerMargin1x);
    const auto labelHeight = s (irSlotLabelHeight1x);
    const auto rowGap = s (irSlotRowGap1x);
    const auto buttonHeight = s (irSlotButtonHeight1x);
    const auto buttonGap = s (irSlotButtonGap1x);
    const auto contentHeight = labelHeight + rowGap + buttonHeight;
    const auto verticalPad = juce::jmax (0, (irBay.getHeight() - contentHeight) / 2);

    const auto layoutSlot = [&] (IrSlot& slot, int slotX)
    {
        const auto slotBounds = juce::Rectangle<int> (slotX, irBay.getY(), halfW, irBay.getHeight()).reduced (innerMargin, 0);

        slot.nameLabel.setBounds (slotBounds.getX(), slotBounds.getY() + verticalPad, slotBounds.getWidth(), labelHeight);

        auto buttonRow = juce::Rectangle<int> (slotBounds.getX(), slotBounds.getY() + verticalPad + labelHeight + rowGap,
                                                slotBounds.getWidth(), buttonHeight);

        // Three equal-width buttons per slot: Browse... (the IR browser
        // overlay), Load IR... (direct file chooser), Default (revert).
        const auto buttonWidth = (buttonRow.getWidth() - 2 * buttonGap) / 3;
        slot.browseButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
        buttonRow.removeFromLeft (buttonGap);
        slot.loadButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
        buttonRow.removeFromLeft (buttonGap);
        slot.defaultButton.setBounds (buttonRow);
    };

    layoutSlot (irSlotA, irBay.getX());
    layoutSlot (irSlotB, irBay.getX() + halfW);

    presetIrNoticeLabel.setBounds (toPlateRect (irNoticeStrip1x));
    presetIrNoticeLabel.setFont (juce::Font (juce::FontOptions {}.withHeight (13.0f * scale)));

    // The browser overlay always spans the full editor (it paints its own
    // scrim + centred panel), at every scale step.
    irBrowserPanel.setBounds (getLocalBounds());
}
