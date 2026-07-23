#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"
#include "gui/ImageDensity.h"
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

    setResizable (false, false);

    const auto storedStep = (int) audioProcessor.apvts.state.getProperty (uiScaleStepProperty, 0);
    applyScaleStep (juce::jlimit (0, (int) scaleSteps.size() - 1, storedStep));
}

NaveAudioProcessorEditor::~NaveAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
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

        const auto loadWidth = (int) std::lround ((float) (buttonRow.getWidth() - buttonGap) * 0.6f);
        slot.loadButton.setBounds (buttonRow.removeFromLeft (loadWidth));
        buttonRow.removeFromLeft (buttonGap);
        slot.defaultButton.setBounds (buttonRow);
    };

    layoutSlot (irSlotA, irBay.getX());
    layoutSlot (irSlotB, irBay.getX() + halfW);
}
