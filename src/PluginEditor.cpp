#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"
#include "ir/IrLibrary.h"
#include "params/ParameterIds.h"
#include "presets/Localisation.h"

#include <BinaryData.h>

#include <cmath>

namespace
{
    using namespace nave::layout;

    juce::Image loadImage (const char* data, int size)
    {
        return juce::ImageCache::getFromMemory (data, size);
    }

    // M2 i18n frame: selects German (resources/i18n/de.txt) or falls
    // through to English, once, at editor construction - see
    // Localisation.h's docs. Called from presetBar's own initialiser
    // expression so installLocalisation() is guaranteed to run before
    // PresetBar's constructor TRANS()es its button labels.
    basilica::presets::PresetManager& initLocalisationThenGetPresetManager (NaveAudioProcessor& processor)
    {
        basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize);
        return processor.presetManager;
    }

    // Non-parameter, per-session UI state: the stepped scale choice (0/1/2)
    // stored as a plain property directly on apvts.state.
    constexpr const char* uiScaleStepProperty = "uiScaleStep";

    // Engraved lettering: gilded antique gold with a dark drop shadow one
    // scaled pixel below (the family typography-pass convention for dark
    // grounds).
    const basilica::gui::EngravedTextStyle plateLabelStyle {
        juce::Colour (0xf0d6ad5e), juce::Colour (0x8c000000), 13.0f, 0.16f, true
    };

    struct SpriteGeometry
    {
        juce::Point<float> anchor;
        float capRadius; // 0 = not a rotating-cap sprite
        float minAngleDeg, maxAngleDeg;
    };

    SpriteGeometry geometryForKind (const juce::String& kind)
    {
        if (kind == "slot")
            return { { slotAnchorX, slotAnchorY }, 0.0f, 0.0f, 0.0f };

        return { { knobAnchorX, knobAnchorY }, knobCapRadius,
                 -knobSweepDeg * 0.5f, knobSweepDeg * 0.5f };
    }
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
      manifest (basilica::gui::LayoutManifest::parse (BinaryData::layout_manifest_json,
                                                      BinaryData::layout_manifest_jsonSize)),
      presetBar (initLocalisationThenGetPresetManager (processorToEdit)),
      typography (BinaryData::EBGaramondRegular_ttf, BinaryData::EBGaramondRegular_ttfSize,
                  BinaryData::EBGaramondSemiBold_ttf, BinaryData::EBGaramondSemiBold_ttfSize)
{
    // For the IR browser overlay + preset bar chrome (the plate itself is
    // fully sprite-composited and needs no LookAndFeel).
    setLookAndFeel (&lookAndFeel);

    // A structurally broken manifest must fail loudly in development and
    // degrade to a plate-only editor in production, never crash.
    jassert (manifest.isValid());

    plateImage = loadImage (BinaryData::plate_nave_png, BinaryData::plate_nave_pngSize);
    knobSprite = loadImage (BinaryData::sprite_knob_brass_png, BinaryData::sprite_knob_brass_pngSize);
    slotSpriteA = loadImage (BinaryData::sprite_ir_slot_a_png, BinaryData::sprite_ir_slot_a_pngSize);
    slotSpriteB = loadImage (BinaryData::sprite_ir_slot_b_png, BinaryData::sprite_ir_slot_b_pngSize);

    // Creation order doubles as the keyboard focus order (JUCE's default
    // FocusTraverser walks children in z-order = creation order): preset
    // bar + scale control first, then the manifest's own reading order
    // (the two cartridge slots, then the knob rows in signal-flow order).
    addAndMakeVisible (presetBar);

    scaleButton.setComponentID ("scaleButton");
    scaleButton.onClick = [this] { cycleScale(); };
    addAndMakeVisible (scaleButton);

    buildControlsFromManifest();

    // The IR browser overlay (issue #1): added LAST so it sits above every
    // other child when visible (z-order follows add order). Hidden until a
    // slot opens it; the editor owns visibility and the slot targeting, the
    // panel owns scanning/filtering/selection (see IrBrowserPanel.h).
    irBrowserPanel.onIrChosen = [this] (const juce::File& irFile)
    {
        const auto loaded = irBrowserTargetSlot == IrSlotId::A
                                 ? audioProcessor.loadImpulseResponseFromFile (irFile)
                                 : audioProcessor.loadImpulseResponseFromFileB (irFile);

        if (loaded)
            if (auto* slot = slotFor (irBrowserTargetSlot))
                refreshSlotName (*slot);
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

        // Hand keyboard focus back to the cartridge slot that opened the
        // overlay, so a keyboard/AT user lands where they left off.
        // grabKeyboardFocus() needs a live native peer - absent in headless
        // tests, hence the guard.
        if (auto* slot = slotFor (irBrowserTargetSlot))
            if (slot->component != nullptr && slot->component->isShowing())
                slot->component->grabKeyboardFocus();
    };
    addChildComponent (irBrowserPanel);

    // Issue #42's non-modal notice. addChildComponent (not
    // addAndMakeVisible): it stays hidden, and takes up none of the plate,
    // until a preset actually reports a missing IR. Non-interactive, so it
    // is not part of the keyboard focus order and cannot swallow a click
    // meant for the art beneath it - but it does carry an accessible title,
    // so a screen-reader user is told the same thing a sighted one is.
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
    audioProcessor.onPresetIrNotice = nullptr;
    setLookAndFeel (nullptr);
}

void NaveAudioProcessorEditor::showPresetIrNotice (const juce::String& message)
{
    presetIrNoticeLabel.setText (message, juce::dontSendNotification);
    presetIrNoticeLabel.setTitle (message);
    presetIrNoticeLabel.setVisible (message.isNotEmpty());
}

juce::Image NaveAudioProcessorEditor::spriteImageFor (const juce::String& spriteKey) const
{
    if (spriteKey == "ir_slot_a")
        return slotSpriteA;

    if (spriteKey == "ir_slot_b")
        return slotSpriteB;

    return knobSprite;
}

void NaveAudioProcessorEditor::buildControlsFromManifest()
{
    for (const auto& entry : manifest.controls)
    {
        if (entry.kind == "slot")
        {
            auto slot = std::make_unique<Slot>();
            slot->entry = &entry;
            slot->id = entry.id == "irSlotA" ? IrSlotId::A : IrSlotId::B;
            slot->label = slot->id == IrSlotId::A ? "IR A" : "IR B";

            slot->component = std::make_unique<basilica::gui::IrCartridgeSlot> (
                spriteImageFor (entry.sprite),
                slot->id == IrSlotId::A ? "Impulse Response A" : "Impulse Response B");

            configureSlotCallbacks (*slot);
            refreshSlotName (*slot);
            addAndMakeVisible (*slot->component);
            slots.push_back (std::move (slot));
            continue;
        }

        auto* parameter = audioProcessor.apvts.getParameter (entry.id);
        jassert (parameter != nullptr); // manifest out of sync with ParameterLayout.cpp
        if (parameter == nullptr)
            continue;

        const auto title = parameter->getName (64);
        const auto geometry = geometryForKind (entry.kind);

        Knob knob;
        knob.entry = &entry;
        knob.slider = std::make_unique<basilica::gui::MasterCropKnob> (
            spriteImageFor (entry.sprite), geometry.anchor, geometry.capRadius, 0.94f,
            geometry.minAngleDeg, geometry.maxAngleDeg);

        knob.slider->setPopupDisplayEnabled (true, true, this);
        knob.slider->setTitle (title);
        knob.slider->setName (title);
        addAndMakeVisible (*knob.slider);

        const auto defaultValue = parameter->getNormalisableRange().convertFrom0to1 (parameter->getDefaultValue());
        knob.slider->setDoubleClickReturnValue (true, defaultValue);

        // SliderAttachment MUST be constructed before the
        // textFromValueFunction override below - JUCE 8.0.14's
        // SliderParameterAttachment constructor itself assigns
        // slider.textFromValueFunction as part of wiring the attachment,
        // which would silently clobber an override set beforehand.
        knob.attachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, entry.id, *knob.slider);

        knob.slider->textFromValueFunction = [parameter] (double v)
        {
            auto text = parameter->getText (parameter->convertTo0to1 ((float) v), 0);
            const auto label = parameter->getLabel();
            return label.isNotEmpty() ? text + " " + label : text;
        };
        knob.slider->updateText();

        knobs.push_back (std::move (knob));
    }
}

void NaveAudioProcessorEditor::configureSlotCallbacks (Slot& slot)
{
    auto* raw = &slot;

    slot.component->onBrowse = [this, raw] { openIrBrowserForSlot (*raw); };
    slot.component->onLoadFile = [this, raw] { chooseImpulseResponseForSlot (*raw); };
    slot.component->onResetToDefault = [this, raw]
    {
        if (raw->id == IrSlotId::A)
            audioProcessor.loadDefaultImpulseResponse();
        else
            audioProcessor.loadDefaultImpulseResponseB();

        refreshSlotName (*raw);
    };
}

void NaveAudioProcessorEditor::refreshSlotName (Slot& slot)
{
    const auto irPath = slot.id == IrSlotId::A ? audioProcessor.getCurrentIrFilePath()
                                               : audioProcessor.getCurrentIrFilePathB();

    slot.component->setIrName (irPath.isEmpty() ? juce::String ("Default")
                                                : juce::File (irPath).getFileNameWithoutExtension());

    // The user has just decided what is in this slot, which settles the
    // question any outstanding "this preset was made with..." notice was
    // asking (issue #42). Leaving it up would nag about a choice already
    // made.
    showPresetIrNotice ({});
}

void NaveAudioProcessorEditor::chooseImpulseResponseForSlot (Slot& slot)
{
    const auto title = slot.id == IrSlotId::A
                            ? "Load a cabinet impulse response..."
                            : "Load a secondary cabinet impulse response (IR B)...";

    slot.activeFileChooser = std::make_unique<juce::FileChooser> (title, juce::File(), "*.wav;*.aiff;*.aif");

    constexpr auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    auto* raw = &slot;
    slot.activeFileChooser->launchAsync (flags, [this, raw] (const juce::FileChooser& chooser)
    {
        const auto file = chooser.getResult();

        if (! file.existsAsFile())
            return;

        const auto loaded = raw->id == IrSlotId::A
                                 ? audioProcessor.loadImpulseResponseFromFile (file)
                                 : audioProcessor.loadImpulseResponseFromFileB (file);

        if (loaded)
            refreshSlotName (*raw);
    });
}

NaveAudioProcessorEditor::Slot* NaveAudioProcessorEditor::slotFor (IrSlotId id) noexcept
{
    for (auto& slot : slots)
        if (slot->id == id)
            return slot.get();

    return nullptr;
}

void NaveAudioProcessorEditor::openIrBrowserForSlot (Slot& slot)
{
    irBrowserTargetSlot = slot.id;

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

    irBrowserPanel.open (slot.label, libraryFolder);
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
    scaleButton.setTitle ("Window scale, " + percentText);

    const auto scale = scaleSteps[(size_t) scaleStepIndex];

    // The IR-name font tracks the slots' drawn size (sprite px -> screen px).
    const auto slotFontHeight = slotNameFontSpritePx * 0.85f * plateToUnit * scale;
    for (auto& slot : slots)
        slot->component->setNameFont (typography.font (slotFontHeight, false, 0.02f));

    setSize ((int) std::lround ((float) baseEditorWidth * scale),
             (int) std::lround ((float) baseEditorHeight * scale));
}

float NaveAudioProcessorEditor::plateScale() const noexcept
{
    return plateToUnit * scaleSteps[(size_t) scaleStepIndex];
}

juce::Point<float> NaveAudioProcessorEditor::plateOrigin() const noexcept
{
    const auto scale = scaleSteps[(size_t) scaleStepIndex];
    return { 0.0f, (float) (topStripHeight1x + topStripGap1x) * scale };
}

void NaveAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);

    const auto scale = scaleSteps[(size_t) scaleStepIndex];

    // Top chrome strip behind the preset bar + scale button.
    const auto stripHeight = (float) topStripHeight1x * scale;
    g.setGradientFill (juce::ColourGradient (juce::Colour (0xff17141a), 0.0f, 0.0f,
                                             juce::Colour (0xff0b090d), 0.0f, stripHeight, false));
    g.fillRect (juce::Rectangle<float> (0.0f, 0.0f, (float) getWidth(), stripHeight));
    g.setColour (juce::Colour (0xff5a4420));
    g.fillRect (juce::Rectangle<float> (0.0f, stripHeight - 1.0f * scale, (float) getWidth(), 1.0f * scale));

    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

    // 1. The empty family plate.
    if (plateImage.isValid())
    {
        const auto origin = plateOrigin();
        g.drawImage (plateImage,
                     juce::Rectangle<float> (origin.x, origin.y,
                                             (float) plateWidth1x * scale, (float) plateHeight1x * scale),
                     juce::RectanglePlacement::stretchToFit, false);
    }

    // 2. Static knob sprites (rotating caps are MasterCropKnob children
    // drawn after this method returns; the cartridge slots draw their own
    // sprite entirely, see IrCartridgeSlot.h).
    drawStaticSprites (g);

    // 3. Engraved lettering - after the sprites so each label sits on top
    // of its control's feathered basalt patch, still under all children.
    drawPlateLettering (g);
}

void NaveAudioProcessorEditor::drawStaticSprites (juce::Graphics& g) const
{
    const auto k = plateScale();
    const auto origin = plateOrigin();

    for (const auto& entry : manifest.controls)
    {
        if (entry.kind == "slot")
            continue; // IrCartridgeSlot children own their full visual

        const auto sprite = spriteImageFor (entry.sprite);
        if (! sprite.isValid())
            continue;

        const auto geometry = geometryForKind (entry.kind);
        const auto drawScale = entry.scale * k;

        const auto transform = juce::AffineTransform::scale (drawScale)
                                   .translated (origin.x + (entry.cx - geometry.anchor.x * entry.scale) * k,
                                                origin.y + (entry.cy - geometry.anchor.y * entry.scale) * k);

        g.drawImageTransformed (sprite, transform);
    }
}

void NaveAudioProcessorEditor::drawPlateLettering (juce::Graphics& g) const
{
    const auto k = plateScale();
    const auto origin = plateOrigin();
    const auto uiScale = scaleSteps[(size_t) scaleStepIndex];

    for (const auto& entry : manifest.controls)
    {
        if (entry.label.isEmpty() || entry.labelCy <= 0.0f)
            continue;

        const juce::Rectangle<float> box (origin.x + (entry.cx - labelBoxWidthPlatePx * 0.5f) * k,
                                          origin.y + (entry.labelCy - labelBoxHeightPlatePx * 0.5f) * k,
                                          labelBoxWidthPlatePx * k,
                                          labelBoxHeightPlatePx * k);

        typography.drawEngraved (g, entry.label, box, uiScale, plateLabelStyle);
    }
}

void NaveAudioProcessorEditor::resized()
{
    const auto uiScale = scaleSteps[(size_t) scaleStepIndex];
    const auto s = [uiScale] (int v) { return (int) std::lround ((float) v * uiScale); };

    auto bounds = getLocalBounds();
    auto topStrip = bounds.removeFromTop (s (topStripHeight1x));

    scaleButton.setBounds (topStrip.removeFromRight (s (scaleButtonWidth1x)).reduced (0, s (2)));
    presetBar.setBounds (topStrip.reduced (0, s (2)));

    const auto k = plateScale();
    const auto origin = plateOrigin();

    for (const auto& knob : knobs)
    {
        const auto& entry = *knob.entry;
        const auto geometry = geometryForKind (entry.kind);

        // Bounds sized to EXACTLY the crop canvas at the sprite's drawn
        // scale, so the rotating cap registers pixel-true on the static
        // sprite underneath (see MasterCropKnob::cropCanvasSizeFor()).
        const auto side = (float) basilica::gui::MasterCropKnob::cropCanvasSizeFor (geometry.capRadius)
                           * entry.scale * k;

        knob.slider->setBounds (juce::Rectangle<float> (side, side)
                                    .withCentre ({ origin.x + entry.cx * k, origin.y + entry.cy * k })
                                    .getSmallestIntegerContainer());
    }

    for (const auto& slot : slots)
    {
        const auto& entry = *slot->entry;

        const auto w = slotSpriteWidthPx * entry.scale * k;
        const auto h = slotSpriteHeightPx * entry.scale * k;

        slot->component->setBounds (juce::Rectangle<float> (w, h)
                                        .withCentre ({ origin.x + entry.cx * k, origin.y + entry.cy * k })
                                        .getSmallestIntegerContainer());
    }

    // Issue #42's notice strip, in plate-local @1x coordinates.
    presetIrNoticeLabel.setBounds (juce::Rectangle<int> (s (irNoticeStrip1x.getX()),
                                                         s (topStripHeight1x + topStripGap1x) + s (irNoticeStrip1x.getY()),
                                                         s (irNoticeStrip1x.getWidth()),
                                                         s (irNoticeStrip1x.getHeight())));
    presetIrNoticeLabel.setFont (juce::Font (juce::FontOptions {}.withHeight (13.0f * uiScale)));

    // The browser overlay always spans the full editor (it paints its own
    // scrim + centred panel), at every scale step.
    irBrowserPanel.setBounds (getLocalBounds());
}
