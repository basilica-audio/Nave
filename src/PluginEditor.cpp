#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "presets/Localisation.h"

#include <BinaryData.h>

namespace
{
    constexpr int knobSize = 100;
    constexpr int textBoxHeight = 20;
    constexpr int labelHeight = 20;
    constexpr int margin = 20;
    constexpr int numKnobs = 8;   // v0.3.0 adds IR B Trim and IR B Delay
    constexpr int choiceRowHeight = 24;
    constexpr int toggleRowHeight = 24;
    constexpr int numChoiceRows = 2;   // 5 combo boxes over two rows
    constexpr int numToggleRows = 1;
    constexpr int irRowHeight = 30;
    constexpr int buttonWidth = 100;
    constexpr int presetBarHeight = 28;
    constexpr int editorWidth = margin * 2 + numKnobs * knobSize + (numKnobs - 1) * margin;
    constexpr int editorHeight = margin * 6 + presetBarHeight + irRowHeight * 2 + labelHeight
                                  + knobSize + textBoxHeight
                                  + numChoiceRows * (choiceRowHeight + margin / 2)
                                  + numToggleRows * (toggleRowHeight + margin / 2);

    // M2 i18n frame (.scaffold/specs/preset-system-m2.md): selects German
    // (resources/i18n/de.txt) or falls through to English, once, at editor
    // construction - see Localisation.h's docs. `presetBar` is a member
    // initialised via the constructor's initialiser list, and its own
    // constructor already calls TRANS() on every button label - member
    // initialisers run in declaration order regardless of the order
    // they're written in, so this helper (called from presetBar's own
    // initialiser expression below) is what actually guarantees
    // installLocalisation() runs before presetBar exists, not a
    // installLocalisation() call in the constructor *body*, which would run
    // too late.
    basilica::presets::PresetManager& initLocalisationThenGetPresetManager (NaveAudioProcessor& processor)
    {
        basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize);
        return processor.presetManager;
    }
}

NaveAudioProcessorEditor::NaveAudioProcessorEditor (NaveAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      audioProcessor (processorToEdit),
      presetBar (initLocalisationThenGetPresetManager (processorToEdit))
{
    addAndMakeVisible (presetBar);

    configureKnob (loCutKnob, ParamIDs::loCut, "LoCut");
    configureKnob (hiCutKnob, ParamIDs::hiCut, "HiCut");
    configureKnob (blendKnob, ParamIDs::irBlend, "IR Blend");
    configureKnob (distanceKnob, ParamIDs::micDistance, "Distance");
    configureKnob (mixKnob, ParamIDs::mix, "Mix");
    configureKnob (levelKnob, ParamIDs::level, "Level");

    configureKnob (irBTrimKnob, ParamIDs::irBTrim, "IR B Trim");
    configureKnob (irBDelayKnob, ParamIDs::irBDelay, "IR B Delay");

    configureChoice (blendModeChoice, ParamIDs::blendMode, "Blend Mode");
    configureChoice (alignModeChoice, ParamIDs::alignMode, "IR Align");
    configureChoice (gainModeChoice, ParamIDs::irGainMode, "IR Gain Match");
    configureChoice (loCutSlopeChoice, ParamIDs::loCutSlope, "LoCut Slope");
    configureChoice (hiCutSlopeChoice, ParamIDs::hiCutSlope, "HiCut Slope");

    configureToggle (irBPolarityToggle, ParamIDs::irBPolarity, "IR B Polarity");
    configureToggle (irAMinPhaseToggle, ParamIDs::irAMinPhase, "IR A Min-Phase");
    configureToggle (irBMinPhaseToggle, ParamIDs::irBMinPhase, "IR B Min-Phase");
    configureToggle (distanceAirToggle, ParamIDs::distanceAir, "Distance Air");

    irNameLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (irNameLabel);
    updateIrLabel();

    loadIrButton.onClick = [this] { chooseImpulseResponseFile(); };
    addAndMakeVisible (loadIrButton);

    defaultIrButton.onClick = [this]
    {
        audioProcessor.loadDefaultImpulseResponse();
        updateIrLabel();
    };
    addAndMakeVisible (defaultIrButton);

    irNameLabelB.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (irNameLabelB);
    updateIrLabelB();

    loadIrButtonB.onClick = [this] { chooseImpulseResponseFileB(); };
    addAndMakeVisible (loadIrButtonB);

    defaultIrButtonB.onClick = [this]
    {
        audioProcessor.loadDefaultImpulseResponseB();
        updateIrLabelB();
    };
    addAndMakeVisible (defaultIrButtonB);

    setResizable (false, false);
    setSize (editorWidth, editorHeight);
}

NaveAudioProcessorEditor::~NaveAudioProcessorEditor() = default;

void NaveAudioProcessorEditor::configureKnob (Knob& knob, const juce::String& parameterId, const juce::String& labelText)
{
    knob.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, knobSize, textBoxHeight);
    addAndMakeVisible (knob.slider);

    knob.label.setText (labelText, juce::dontSendNotification);
    knob.label.setJustificationType (juce::Justification::centred);
    // false => label sits above the slider it tracks; JUCE repositions it
    // automatically whenever the slider's bounds change, so resized() only
    // needs to place the sliders themselves.
    knob.label.attachToComponent (&knob.slider, false);
    addAndMakeVisible (knob.label);

    knob.attachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, parameterId, knob.slider);
}

void NaveAudioProcessorEditor::configureChoice (Choice& choice,
                                                 const juce::String& parameterId,
                                                 const juce::String& labelText)
{
    // ComboBoxAttachment does NOT populate the box (JUCE 8.0.14): it only
    // synchronises the selected index. The items have to come from the
    // parameter's own choice list first, or the attachment binds to an empty
    // box and the control appears blank.
    if (auto* parameter = dynamic_cast<juce::AudioParameterChoice*> (
            audioProcessor.apvts.getParameter (parameterId)))
    {
        choice.comboBox.addItemList (parameter->choices, 1);
    }

    addAndMakeVisible (choice.comboBox);

    choice.label.setText (labelText, juce::dontSendNotification);
    choice.label.setJustificationType (juce::Justification::centredRight);
    addAndMakeVisible (choice.label);

    // Constructed last, so it finds a populated box to attach to.
    choice.attachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, parameterId, choice.comboBox);
}

void NaveAudioProcessorEditor::configureToggle (Toggle& toggle,
                                                 const juce::String& parameterId,
                                                 const juce::String& labelText)
{
    toggle.button.setButtonText (labelText);
    addAndMakeVisible (toggle.button);

    toggle.attachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, parameterId, toggle.button);
}

void NaveAudioProcessorEditor::updateIrLabel()
{
    const auto irPath = audioProcessor.getCurrentIrFilePath();

    irNameLabel.setText (irPath.isEmpty() ? "IR A: Default (no IR loaded)" : "IR A: " + juce::File (irPath).getFileName(),
                          juce::dontSendNotification);
}

void NaveAudioProcessorEditor::updateIrLabelB()
{
    const auto irPath = audioProcessor.getCurrentIrFilePathB();

    irNameLabelB.setText (irPath.isEmpty() ? "IR B: Default (no IR loaded)" : "IR B: " + juce::File (irPath).getFileName(),
                           juce::dontSendNotification);
}

void NaveAudioProcessorEditor::chooseImpulseResponseFile()
{
    activeFileChooser = std::make_unique<juce::FileChooser> (
        "Load a cabinet impulse response...",
        juce::File(),
        "*.wav;*.aiff;*.aif");

    constexpr auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    activeFileChooser->launchAsync (flags, [this] (const juce::FileChooser& chooser)
    {
        const auto file = chooser.getResult();

        if (file.existsAsFile())
        {
            audioProcessor.loadImpulseResponseFromFile (file);
            updateIrLabel();
        }
    });
}

void NaveAudioProcessorEditor::chooseImpulseResponseFileB()
{
    activeFileChooserB = std::make_unique<juce::FileChooser> (
        "Load a secondary cabinet impulse response (IR B)...",
        juce::File(),
        "*.wav;*.aiff;*.aif");

    constexpr auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    activeFileChooserB->launchAsync (flags, [this] (const juce::FileChooser& chooser)
    {
        const auto file = chooser.getResult();

        if (file.existsAsFile())
        {
            audioProcessor.loadImpulseResponseFromFileB (file);
            updateIrLabelB();
        }
    });
}

void NaveAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (margin);

    presetBar.setBounds (bounds.removeFromTop (presetBarHeight));
    bounds.removeFromTop (margin);

    auto irRow = bounds.removeFromTop (irRowHeight);
    defaultIrButton.setBounds (irRow.removeFromRight (buttonWidth));
    irRow.removeFromRight (margin / 2);
    loadIrButton.setBounds (irRow.removeFromRight (buttonWidth));
    irRow.removeFromRight (margin / 2);
    irNameLabel.setBounds (irRow);

    bounds.removeFromTop (margin / 2);

    auto irRowB = bounds.removeFromTop (irRowHeight);
    defaultIrButtonB.setBounds (irRowB.removeFromRight (buttonWidth));
    irRowB.removeFromRight (margin / 2);
    loadIrButtonB.setBounds (irRowB.removeFromRight (buttonWidth));
    irRowB.removeFromRight (margin / 2);
    irNameLabelB.setBounds (irRowB);

    bounds.removeFromTop (margin / 2);

    // Two rows of choice controls, then one row of toggles, above the knobs.
    const auto layoutChoiceRow = [&] (std::initializer_list<Choice*> row)
    {
        auto rowBounds = bounds.removeFromTop (choiceRowHeight);
        const auto cellWidth = rowBounds.getWidth() / static_cast<int> (row.size());

        for (auto* choice : row)
        {
            auto cell = rowBounds.removeFromLeft (cellWidth).reduced (margin / 4, 0);
            choice->label.setBounds (cell.removeFromLeft (cell.getWidth() / 2));
            choice->comboBox.setBounds (cell);
        }

        bounds.removeFromTop (margin / 2);
    };

    layoutChoiceRow ({ &blendModeChoice, &alignModeChoice, &gainModeChoice });
    layoutChoiceRow ({ &loCutSlopeChoice, &hiCutSlopeChoice });

    {
        auto toggleRow = bounds.removeFromTop (toggleRowHeight);
        Toggle* toggles[] = { &irBPolarityToggle, &irAMinPhaseToggle, &irBMinPhaseToggle, &distanceAirToggle };
        const auto cellWidth = toggleRow.getWidth() / static_cast<int> (std::size (toggles));

        for (auto* toggle : toggles)
            toggle->button.setBounds (toggleRow.removeFromLeft (cellWidth).reduced (margin / 4, 0));

        bounds.removeFromTop (margin / 2);
    }

    bounds.removeFromTop (labelHeight); // room for the attached labels above each knob

    const auto slotWidth = bounds.getWidth() / numKnobs;

    for (auto* knob : { &loCutKnob, &hiCutKnob, &blendKnob, &distanceKnob,
                         &irBTrimKnob, &irBDelayKnob, &mixKnob, &levelKnob })
        knob->slider.setBounds (bounds.removeFromLeft (slotWidth).reduced (margin / 2, 0));
}
