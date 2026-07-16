#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "presets/PresetBar.h"

class NaveAudioProcessor;

// A simple, functional v0.1 editor: one rotary slider per parameter, bound
// to the APVTS via SliderAttachment, plus a "Load IR..."/"Default" button
// pair for choosing/clearing the cabinet impulse response. A custom
// vector-drawn GUI is a later milestone; this is deliberately plain but
// fully wired and usable.
class NaveAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit NaveAudioProcessorEditor (NaveAudioProcessor& processorToEdit);
    ~NaveAudioProcessorEditor() override;

    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    // One knob + label per parameter, in signal-flow order.
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    void configureKnob (Knob& knob, const juce::String& parameterId, const juce::String& labelText);
    void updateIrLabel();
    void updateIrLabelB();
    void chooseImpulseResponseFile();
    void chooseImpulseResponseFileB();

    NaveAudioProcessor& audioProcessor;

    // M2 preset system (src/presets/PresetBar.h) - a horizontal strip
    // docked at the top of the editor. Constructed after the localisation
    // frame is installed (see the constructor) so its TRANS()'d strings
    // (and any of its own dialogs opened later) pick up the right language
    // from the very first paint.
    basilica::presets::PresetBar presetBar;

    Knob loCutKnob;
    Knob hiCutKnob;
    Knob blendKnob;
    Knob distanceKnob;
    Knob mixKnob;
    Knob levelKnob;

    juce::Label irNameLabel;
    juce::TextButton loadIrButton { "Load IR..." };
    juce::TextButton defaultIrButton { "Default" };

    juce::Label irNameLabelB;
    juce::TextButton loadIrButtonB { "Load IR B..." };
    juce::TextButton defaultIrButtonB { "Default" };

    std::unique_ptr<juce::FileChooser> activeFileChooser;
    std::unique_ptr<juce::FileChooser> activeFileChooserB;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NaveAudioProcessorEditor)
};
