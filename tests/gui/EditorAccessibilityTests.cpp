#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "gui/IrCartridgeSlot.h"
#include "gui/MasterCropKnob.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Accessibility tests for the wave-3 compositional editor, carrying over
// the suite's M3 a11y review contract: assert the actual
// AccessibilityHandler-level behaviour, not just that the editor
// constructs. juce::ScopedJuceInitialiser_GUI is installed once for the
// whole test binary in tests/TestMain.cpp.
//
// createAccessibilityHandler() is called directly rather than
// getAccessibilityHandler(): the latter (JUCE 8.0.14
// juce_Component.cpp:3323-3326) only returns a handler once the component
// has a live native window peer, which this headless test binary never
// has.
namespace
{
    template <typename ComponentType>
    ComponentType* findChildByTitle (juce::Component& parent, const juce::String& title)
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
            if (auto* typed = dynamic_cast<ComponentType*> (parent.getChildComponent (i)))
                if (typed->getTitle() == title)
                    return typed;

        return nullptr;
    }

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
        const char* title;
        const char* unitSuffix;
    };

    const Expectation expectations[] = {
        { "LoCut", "Hz" },
        { "IR Blend", "%" },
        { "Level", "dB" },
    };

    for (const auto& expectation : expectations)
    {
        auto* knob = findChildByTitle<basilica::gui::MasterCropKnob> (editor, expectation.title);
        REQUIRE (knob != nullptr);

        const auto handler = createHandlerForTest (*knob);
        REQUIRE (handler != nullptr);

        auto* valueInterface = handler->getValueInterface();
        REQUIRE (valueInterface != nullptr);

        const auto valueText = valueInterface->getCurrentValueAsString();
        INFO ("knob \"" << expectation.title << "\" accessible value = \"" << valueText.toStdString() << "\"");
        CHECK (valueText.endsWith (expectation.unitSuffix));
    }
}

TEST_CASE ("Cartridge slots expose slot-specific titles and the loaded IR as their accessible value", "[gui][a11y]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    for (const auto* title : { "Impulse Response A", "Impulse Response B" })
    {
        auto* slot = findChildByTitle<basilica::gui::IrCartridgeSlot> (editor, title);
        INFO ("slot \"" << title << "\"");
        REQUIRE (slot != nullptr);

        CHECK (slot->getWantsKeyboardFocus());

        const auto handler = createHandlerForTest (*slot);
        REQUIRE (handler != nullptr);
        CHECK (handler->getRole() == juce::AccessibilityRole::button);

        auto* valueInterface = handler->getValueInterface();
        REQUIRE (valueInterface != nullptr);
        CHECK (valueInterface->isReadOnly());

        // A fresh processor holds the built-in unit impulse in both slots.
        CHECK (valueInterface->getCurrentValueAsString() == "Default");
    }
}

TEST_CASE ("Resetting a slot to default through the cartridge action updates its shown IR name", "[gui][a11y]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    auto* slot = findChildByTitle<basilica::gui::IrCartridgeSlot> (editor, "Impulse Response A");
    REQUIRE (slot != nullptr);

    // The reset action must run the REAL processor path and re-publish the
    // name - the operability contract, exercised end to end without a
    // native file dialog.
    slot->performResetToDefault();
    CHECK (slot->irName_forTest() == "Default");
    CHECK (processor.getCurrentIrFilePath().isEmpty());
}

TEST_CASE ("The cartridge browse gesture opens the IR browser overlay", "[gui][a11y]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    auto* browser = dynamic_cast<basilica::gui::IrBrowserPanel*> (
        editor.findChildWithID ("irBrowserPanel"));

    // The overlay carries no componentID in this generation - find it by
    // type instead.
    if (browser == nullptr)
        for (int i = 0; i < editor.getNumChildComponents() && browser == nullptr; ++i)
            browser = dynamic_cast<basilica::gui::IrBrowserPanel*> (editor.getChildComponent (i));

    REQUIRE (browser != nullptr);
    CHECK_FALSE (browser->isVisible());

    auto* slot = findChildByTitle<basilica::gui::IrCartridgeSlot> (editor, "Impulse Response B");
    REQUIRE (slot != nullptr);

    slot->performBrowse();
    CHECK (browser->isVisible());
}

TEST_CASE ("Scale button's accessible title reflects the current scale percentage, not a static string", "[gui][a11y]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    auto* scaleButton = dynamic_cast<juce::TextButton*> (editor.findChildWithID ("scaleButton"));
    REQUIRE (scaleButton != nullptr);

    CHECK (scaleButton->getTitle().contains ("100%"));

    REQUIRE (scaleButton->onClick);
    scaleButton->onClick();

    CHECK (scaleButton->getButtonText() == "150%");
    CHECK (scaleButton->getTitle().contains ("150%"));
    CHECK_FALSE (scaleButton->getTitle().contains ("100%"));
}

TEST_CASE ("Every interactive control is keyboard-focusable", "[gui][a11y]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    int slidersSeen = 0, slotsSeen = 0;

    for (int i = 0; i < editor.getNumChildComponents(); ++i)
    {
        auto* child = editor.getChildComponent (i);

        if (auto* slider = dynamic_cast<juce::Slider*> (child))
        {
            ++slidersSeen;
            INFO ("slider \"" << slider->getTitle().toStdString() << "\"");
            CHECK (slider->getWantsKeyboardFocus());
        }
        else if (auto* slot = dynamic_cast<basilica::gui::IrCartridgeSlot*> (child))
        {
            ++slotsSeen;
            CHECK (slot->getWantsKeyboardFocus());
        }
    }

    // All 6 knobs are sliders; the two cartridge slots are focusable
    // buttons. A zero-match loop must not pass vacuously.
    CHECK (slidersSeen == 6);
    CHECK (slotsSeen == 2);

    auto* scaleButton = editor.findChildWithID ("scaleButton");
    REQUIRE (scaleButton != nullptr);
    CHECK (scaleButton->getWantsKeyboardFocus());
}

TEST_CASE ("Arrow keys step knobs by a practical amount, Shift+Arrow steps finer", "[gui][a11y]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    auto* knob = findChildByTitle<basilica::gui::MasterCropKnob> (editor, "IR Blend");
    REQUIRE (knob != nullptr);

    const auto range = knob->getMaximum() - knob->getMinimum();
    knob->setValue (knob->getMinimum() + range * 0.5, juce::dontSendNotification);
    const auto before = knob->getValue();

    REQUIRE (knob->keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    const auto coarseStep = knob->getValue() - before;

    CHECK (coarseStep > range * 0.005);
    CHECK (coarseStep < range * 0.02);

    const auto beforeFine = knob->getValue();
    REQUIRE (knob->keyPressed (juce::KeyPress (juce::KeyPress::rightKey,
                                               juce::ModifierKeys::shiftModifier, 0)));
    const auto fineStep = knob->getValue() - beforeFine;

    CHECK (fineStep > 0.0);
    CHECK (fineStep < coarseStep);
}
