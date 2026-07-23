#include "PluginEditor.h"
#include "PluginProcessor.h"

#include <catch2/catch_test_macros.hpp>

// M3 GUI accessibility tests, following the same pattern silentium's M3
// pilot established (tests/gui/EditorAccessibilityTests.cpp there,
// originally the M3 a11y review's A-01/A-02/A-05/A-07 follow-ups): assert
// actual AccessibilityHandler-level behaviour, not just that the editor
// constructs without crashing (EditorSnapshotTests.cpp already covers
// that). juce::ScopedJuceInitialiser_GUI is installed once for the whole
// test binary in tests/TestMain.cpp, so constructing Components is safe
// here even though this is a headless console executable with no running
// message loop or native window/peer.
//
// Deliberately calls createAccessibilityHandler() directly rather than the
// more commonly used getAccessibilityHandler(): the latter (JUCE 8.0.14
// juce_Component.cpp) only returns a handler once the component has a live
// native window peer, which this headless, no-message-loop test binary
// never has. createAccessibilityHandler() is public API specifically meant
// to be safely callable/overridable independent of any live OS
// accessibility bridge.
namespace
{
    template <typename ComponentType>
    ComponentType* findChildByTitle (juce::Component& parent, const juce::String& title)
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
        {
            if (auto* typed = dynamic_cast<ComponentType*> (parent.getChildComponent (i)))
                if (typed->getTitle() == title)
                    return typed;
        }

        return nullptr;
    }

    // juce::Button::createAccessibilityHandler() (unlike juce::Slider's) is
    // declared PROTECTED (JUCE 8.0.14 juce_Button.h) - calling it through a
    // juce::TextButton*/juce::Button* would fail to compile even though
    // it's the exact same public virtual originally declared on
    // juce::Component. Per the C++ standard's access-control-for-virtual-
    // calls rule ([class.access.virt]), access is checked against the
    // STATIC type used to name the call, not the dynamic override - calling
    // through a juce::Component& (where the function is public) compiles,
    // and virtual dispatch still correctly invokes the most-derived
    // override at runtime.
    std::unique_ptr<juce::AccessibilityHandler> createHandlerForTest (juce::Component& component)
    {
        return component.createAccessibilityHandler();
    }
}

TEST_CASE ("Knob accessibility value strings include their declared unit", "[gui][a11y]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    struct Expectation
    {
        const char* label;
        const char* unitSuffix;
    };

    // One representative knob per unit declared in ParameterLayout.cpp
    // (.withLabel("Hz"/"%"/"dB")).
    const Expectation expectations[] = {
        { "LoCut", "Hz" },
        { "IR Blend", "%" },
        { "Level", "dB" },
    };

    for (const auto& expectation : expectations)
    {
        auto* knob = findChildByTitle<basilica::gui::FilmstripKnob> (editor, expectation.label);
        REQUIRE (knob != nullptr);

        const auto handler = createHandlerForTest (*knob);
        REQUIRE (handler != nullptr);

        auto* valueInterface = handler->getValueInterface();
        REQUIRE (valueInterface != nullptr);

        const auto valueText = valueInterface->getCurrentValueAsString();
        INFO ("knob \"" << expectation.label << "\" accessible value = \"" << valueText.toStdString() << "\"");
        CHECK (valueText.endsWith (expectation.unitSuffix));
    }
}

TEST_CASE ("Scale button's accessible title reflects the current scale percentage, not a static string", "[gui][a11y]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    auto* scaleButton = dynamic_cast<juce::TextButton*> (editor.findChildWithID ("scaleButton"));
    REQUIRE (scaleButton != nullptr);

    CHECK (scaleButton->getTitle().contains ("100%"));

    // Cycle the scale via the SAME onClick callback a mouse/keyboard/AT
    // click would invoke - called directly rather than via triggerClick(),
    // which only posts an async command message (JUCE 8.0.14
    // juce_Button.cpp) that would need a running message loop to ever
    // actually fire, which this headless test binary doesn't have.
    REQUIRE (scaleButton->onClick);
    scaleButton->onClick();

    CHECK (scaleButton->getButtonText() == "150%");
    CHECK (scaleButton->getTitle().contains ("150%"));
    CHECK_FALSE (scaleButton->getTitle().contains ("100%"));
}

TEST_CASE ("IR loader slot buttons expose readable, slot-specific, keyboard-operable accessible names", "[gui][a11y]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    for (const auto* idPrefix : { "irSlotA", "irSlotB" })
    {
        auto* loadButton = dynamic_cast<juce::TextButton*> (editor.findChildWithID (juce::String (idPrefix) + ".loadButton"));
        auto* defaultButton = dynamic_cast<juce::TextButton*> (editor.findChildWithID (juce::String (idPrefix) + ".defaultButton"));

        REQUIRE (loadButton != nullptr);
        REQUIRE (defaultButton != nullptr);

        // Titles must be slot-specific ("IR A"/"IR B"), not a generic
        // "Load IR..." shared across both buttons - otherwise an AT user
        // navigating by name alone couldn't tell the two slots apart.
        const auto expectedSlotName = juce::String (idPrefix) == "irSlotA" ? juce::String ("IR A") : juce::String ("IR B");
        CHECK (loadButton->getTitle().contains (expectedSlotName));
        CHECK (defaultButton->getTitle().contains (expectedSlotName));

        // Both are plain juce::TextButtons - JUCE's default ButtonAccessibilityHandler
        // exposes a press action and non-toggleable state, confirming they are
        // reachable/operable by assistive technology and keyboard (Enter/Space
        // trigger onClick via the same button-press action a screen reader
        // invokes) without requiring the suite's custom paintFocusRing()
        // machinery FilmstripKnob/FilmstripToggle need (see PluginEditor.h's
        // IrSlot docs - LookAndFeel_V4::drawButtonBackground already boosts
        // saturation on keyboard focus for standard buttons).
        const auto loadHandler = createHandlerForTest (*loadButton);
        REQUIRE (loadHandler != nullptr);
        CHECK (loadHandler->getActions().contains (juce::AccessibilityActionType::press));
    }
}

TEST_CASE ("IR loader slot name labels reflect the current IR state and update when Default is clicked", "[gui][a11y]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    auto* nameLabelA = dynamic_cast<juce::Label*> (editor.findChildWithID ("irSlotA.nameLabel"));
    REQUIRE (nameLabelA != nullptr);
    CHECK (nameLabelA->getText().startsWith ("IR A:"));
    CHECK (nameLabelA->getText().contains ("Default"));
    CHECK (nameLabelA->getTitle() == nameLabelA->getText());

    auto* defaultButtonA = dynamic_cast<juce::TextButton*> (editor.findChildWithID ("irSlotA.defaultButton"));
    REQUIRE (defaultButtonA != nullptr);
    REQUIRE (defaultButtonA->onClick);

    // Exercises the same refreshIrSlotLabel() path a real click would -
    // called directly for the same headless/no-message-loop reason the
    // scale button test above calls onClick() directly rather than
    // triggerClick().
    defaultButtonA->onClick();
    CHECK (nameLabelA->getText().startsWith ("IR A:"));
    CHECK (nameLabelA->getTitle() == nameLabelA->getText());
}
