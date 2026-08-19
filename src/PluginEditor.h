#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>

#include "gui/BasilicaLookAndFeel.h"
#include "gui/FilmstripKnob.h"
#include "presets/PresetBar.h"

class NaveAudioProcessor;

// M3 GUI pass: Nave's photoreal skeuomorphic editor, built from the suite's
// reusable src/gui/ component family (FilmstripKnob, BasilicaLookAndFeel -
// see also AnalogMeter/FilmstripToggle, copied verbatim into src/gui/ for
// suite consistency even though Nave's own layout has no meter or toggle
// bays) plus the pre-rendered faceplate PNG (see
// .scaffold/gui-assets/faceplate-nave-v1/README.md). Every FilmstripKnob is
// wired to a real APVTS parameter; the ir_loader bay's controls are wired to
// real (non-APVTS) IR-file-slot state on the processor - no dead decoration.
//
// Layout: a single "knobLayout" table plus dedicated ir_loader bay wiring
// (see PluginEditor.cpp), positioned from the base-resolution coordinates in
// PluginEditorLayout.h (nave::layout), mirroring silentium's M3 pilot
// pattern (src/PluginEditorLayout.h there, slnt::layout).
//
// Window scaling is STEPPED (100/150/200%, a UA-style corner control next to
// the preset bar, persisted as a plain property on the APVTS state tree),
// matching every other M3-complete plugin in the suite.
class NaveAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit NaveAudioProcessorEditor (NaveAudioProcessor& processorToEdit);
    ~NaveAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    struct Knob
    {
        std::unique_ptr<basilica::gui::FilmstripKnob> slider;
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    // Which of the two independent IR slots (see ParamIDs::
    // irFilePathProperty/irFilePathBProperty's docs - plain ValueTree
    // properties, not APVTS parameters) this IrSlot instance controls.
    enum class IrSlotId
    {
        A,
        B
    };

    // One IR loader "slot": a read-only name label showing the currently
    // loaded file (or "Default"), a "Load IR..." button that opens a
    // FileChooser, and a "Default" button that reverts to the built-in
    // unit-impulse IR. Styled entirely via BasilicaLookAndFeel's default
    // juce::TextButton/juce::Label drawing (JUCE 8.0.14's LookAndFeel_V4
    // dark colour scheme, which BasilicaLookAndFeel inherits unmodified -
    // already reasonably readable against the faceplate's dark stone
    // background, and LookAndFeel_V4::drawButtonBackground boosts the
    // button's saturation on keyboard focus, giving standard TextButtons a
    // built-in focus affordance the suite's custom-painted FilmstripKnob/
    // FilmstripToggle need paintFocusRing() to replicate - see
    // BasilicaLookAndFeel.h's docs).
    struct IrSlot
    {
        juce::Label nameLabel;
        juce::TextButton loadButton;
        juce::TextButton defaultButton;
        std::unique_ptr<juce::FileChooser> activeFileChooser;
    };

    void configureKnob (Knob& knob, const juce::String& parameterId, const juce::String& labelText);
    void configureIrSlot (IrSlot& slot, IrSlotId id, const juce::String& slotLabel);
    void refreshIrSlotLabel (IrSlot& slot, IrSlotId id, const juce::String& slotLabel);
    void chooseImpulseResponseForSlot (IrSlot& slot, IrSlotId id, const juce::String& slotLabel);
    void applyScaleStep (int newStepIndex);
    void cycleScale();

    NaveAudioProcessor& audioProcessor;

    basilica::gui::BasilicaLookAndFeel lookAndFeel;

    juce::Image facePlateImage1x, facePlateImage2x;
    juce::Image brandIconImage;

    basilica::presets::PresetBar presetBar;
    juce::TextButton scaleButton;
    int scaleStepIndex = 0; // 0 = 100%, 1 = 150%, 2 = 200%

    static constexpr int numKnobs = 6;
    std::array<Knob, numKnobs> knobs;

    IrSlot irSlotA;
    IrSlot irSlotB;

    juce::Label titleLabel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NaveAudioProcessorEditor)
};
