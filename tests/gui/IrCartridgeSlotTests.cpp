#include "gui/IrCartridgeSlot.h"

#include <catch2/catch_test_macros.hpp>

// Component tests for the D1 cartridge slot's gesture mapping and action
// surface (gui/IrCartridgeSlot.h). The end-to-end wiring proofs (browser
// opens, default reset reaches the processor) live in
// tests/gui/EditorAccessibilityTests.cpp against the real editor tree.

TEST_CASE ("Sprite-space gesture mapping: window browses, button loads, margins are dead", "[gui][slot]")
{
    using S = basilica::gui::IrCartridgeSlot;

    // Centre of the smoked-glass window -> browse.
    CHECK (S::gestureForPoint ({ 0.5f * (S::windowLeftPx + S::windowRightPx),
                                 0.5f * (S::windowTopPx + S::windowBottomPx) }) == S::Gesture::browse);

    // The brass frame outside the window still browses (big target).
    CHECK (S::gestureForPoint ({ S::frameLeftPx + 10.0f, S::frameTopPx + 10.0f }) == S::Gesture::browse);

    // The round load button -> direct file chooser.
    CHECK (S::gestureForPoint ({ S::buttonCentreXPx, S::buttonCentreYPx }) == S::Gesture::loadFile);
    CHECK (S::gestureForPoint ({ S::buttonCentreXPx + S::buttonHitRadiusPx - 1.0f,
                                 S::buttonCentreYPx }) == S::Gesture::loadFile);

    // The blended basalt margin is visually dead space - clicks fall through.
    CHECK (S::gestureForPoint ({ 2.0f, 2.0f }) == S::Gesture::none);
    CHECK (S::gestureForPoint ({ S::spriteWidthPx - 2.0f, S::spriteHeightPx - 2.0f }) == S::Gesture::none);
}

TEST_CASE ("Public actions invoke exactly their own callback", "[gui][slot]")
{
    basilica::gui::IrCartridgeSlot slot (juce::Image {}, "Impulse Response A");

    int browses = 0, loads = 0, resets = 0;
    slot.onBrowse = [&browses] { ++browses; };
    slot.onLoadFile = [&loads] { ++loads; };
    slot.onResetToDefault = [&resets] { ++resets; };

    slot.performBrowse();
    slot.performLoadFile();
    slot.performLoadFile();
    slot.performResetToDefault();

    CHECK (browses == 1);
    CHECK (loads == 2);
    CHECK (resets == 1);
}

TEST_CASE ("Return and Space trigger the browse gesture from the keyboard", "[gui][slot]")
{
    basilica::gui::IrCartridgeSlot slot (juce::Image {}, "Impulse Response B");

    int browses = 0;
    slot.onBrowse = [&browses] { ++browses; };

    CHECK (slot.keyPressed (juce::KeyPress (juce::KeyPress::returnKey)));
    CHECK (slot.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey)));
    CHECK_FALSE (slot.keyPressed (juce::KeyPress (juce::KeyPress::leftKey)));

    CHECK (browses == 2);
}

TEST_CASE ("The accessible value is the published IR name", "[gui][slot]")
{
    basilica::gui::IrCartridgeSlot slot (juce::Image {}, "Impulse Response A");

    slot.setIrName ("modelled_1x15_vintage");

    const auto handler = slot.createAccessibilityHandler();
    REQUIRE (handler != nullptr);

    auto* valueInterface = handler->getValueInterface();
    REQUIRE (valueInterface != nullptr);
    CHECK (valueInterface->isReadOnly());
    CHECK (valueInterface->getCurrentValueAsString() == "modelled_1x15_vintage");
}
