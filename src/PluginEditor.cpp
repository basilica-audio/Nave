#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "params/ParameterIds.h"

namespace
{
    constexpr int knobSize = 100;
    constexpr int textBoxHeight = 20;
    constexpr int labelHeight = 20;
    constexpr int margin = 20;
    constexpr int numKnobs = 6;
    constexpr int irRowHeight = 30;
    constexpr int buttonWidth = 100;
    constexpr int editorWidth = margin * 2 + numKnobs * knobSize + (numKnobs - 1) * margin;
    constexpr int editorHeight = margin * 4 + irRowHeight * 2 + labelHeight + knobSize + textBoxHeight;
}

NaveAudioProcessorEditor::NaveAudioProcessorEditor (NaveAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      audioProcessor (processorToEdit)
{
    configureKnob (loCutKnob, ParamIDs::loCut, "LoCut");
    configureKnob (hiCutKnob, ParamIDs::hiCut, "HiCut");
    configureKnob (blendKnob, ParamIDs::irBlend, "IR Blend");
    configureKnob (distanceKnob, ParamIDs::micDistance, "Distance");
    configureKnob (mixKnob, ParamIDs::mix, "Mix");
    configureKnob (levelKnob, ParamIDs::level, "Level");

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

    bounds.removeFromTop (margin);
    bounds.removeFromTop (labelHeight); // room for the attached labels above each knob

    const auto slotWidth = bounds.getWidth() / numKnobs;

    for (auto* knob : { &loCutKnob, &hiCutKnob, &blendKnob, &distanceKnob, &mixKnob, &levelKnob })
        knob->slider.setBounds (bounds.removeFromLeft (slotWidth).reduced (margin / 2, 0));
}
