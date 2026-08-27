#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// D1 IR cartridge slot (DECISIONS.md D1, binding 2026-07-31): a recessed
// brass cartridge frame with a dark smoked-glass window and a small round
// brass load button, extracted as a sprite from the accepted nave wave-1
// base render (sprite-library/d1-cartridge-slot-{a,b}.png - the engraved
// A/B marker is baked into each sprite). Per D1, the sprite bakes the
// EMPTY slot + button only; JUCE overlays the loaded IR's name as
// warm-gold text in the window plus all click handling - text is never
// baked into renders.
//
// OPERABILITY CONTRACT (the suite's no-dead-decoration rule): every baked
// affordance really works -
//   * the smoked-glass window (and the frame around it) opens the IR
//     BROWSER for this slot (auditioning is the primary gesture, same
//     rationale as the old editor's Browse... button),
//   * the round brass load button opens the direct FILE chooser,
//   * right-click (or the keyboard context-menu key path via the popup
//     menu) offers Browse... / Load file... / Reset to default,
//   * Return/Space with keyboard focus opens the browser.
// The owner supplies the three actions as callbacks and pushes the
// currently loaded IR's display name via setIrName() - this component
// holds no IR state of its own.
//
// The gesture hit zones are expressed in SPRITE pixel space and resolved
// through gestureForPoint(), a pure function, so tests can assert the
// mapping without synthesising mouse events.
namespace basilica::gui
{
    class IrCartridgeSlot : public juce::Component
    {
    public:
        // Hit-zone geometry, measured once against the 522x207 slot
        // sprites (both A and B share it - same source render row):
        // frame ~(25,25)-(495,180), glass window ~(90,68)-(390,144),
        // round load button centre ~(457,104) r ~20 (hit radius 24).
        static constexpr float spriteWidthPx = 522.0f;
        static constexpr float spriteHeightPx = 207.0f;
        static constexpr float windowLeftPx = 90.0f, windowTopPx = 68.0f;
        static constexpr float windowRightPx = 390.0f, windowBottomPx = 144.0f;
        static constexpr float buttonCentreXPx = 457.0f, buttonCentreYPx = 104.0f;
        static constexpr float buttonHitRadiusPx = 24.0f;
        static constexpr float frameLeftPx = 25.0f, frameTopPx = 25.0f;
        static constexpr float frameRightPx = 497.0f, frameBottomPx = 182.0f;

        enum class Gesture
        {
            none,     // blended basalt margin - clicks fall through visually dead space
            browse,   // glass window / frame body -> IR browser
            loadFile, // round brass button -> direct file chooser
        };

        // Pure sprite-space classifier shared by the mouse path and tests.
        static Gesture gestureForPoint (juce::Point<float> spritePoint) noexcept
        {
            if (spritePoint.getDistanceFrom ({ buttonCentreXPx, buttonCentreYPx }) <= buttonHitRadiusPx)
                return Gesture::loadFile;

            const juce::Rectangle<float> frame (frameLeftPx, frameTopPx,
                                                frameRightPx - frameLeftPx, frameBottomPx - frameTopPx);
            return frame.contains (spritePoint) ? Gesture::browse : Gesture::none;
        }

        IrCartridgeSlot (juce::Image spriteImage, juce::String accessibleTitle)
            : sprite (std::move (spriteImage)), title (std::move (accessibleTitle))
        {
            setTitle (title);
            setDescription (title);
            setWantsKeyboardFocus (true);
        }

        std::function<void()> onBrowse;
        std::function<void()> onLoadFile;
        std::function<void()> onResetToDefault;

        // The display name shown in the smoked-glass window ("Default (no
        // IR loaded)" when empty) - pushed by the owner whenever the
        // processor's slot state changes.
        void setIrName (const juce::String& newName)
        {
            irName = newName;
            setHelpText (irName);
            repaint();
        }

        const juce::String& irName_forTest() const noexcept { return irName; }

        void setNameFont (juce::Font newFont)
        {
            nameFont = std::move (newFont);
            repaint();
        }

        // The three gestures as public actions, so the mouse path, the
        // popup menu, the keyboard path and the tests all run through the
        // exact same code.
        void performBrowse()          { if (onBrowse) onBrowse(); }
        void performLoadFile()        { if (onLoadFile) onLoadFile(); }
        void performResetToDefault()  { if (onResetToDefault) onResetToDefault(); }

        void paint (juce::Graphics& g) override
        {
            if (! sprite.isValid())
                return;

            g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

            const auto scale = spriteToLocalScale();
            g.drawImageTransformed (sprite, juce::AffineTransform::scale (scale));

            // The loaded IR's name, warm gold in the smoked-glass window
            // (D1: the only dynamic content is text, rendered by JUCE).
            const juce::Rectangle<float> window (windowLeftPx * scale, windowTopPx * scale,
                                                 (windowRightPx - windowLeftPx) * scale,
                                                 (windowBottomPx - windowTopPx) * scale);

            g.setFont (nameFont);
            g.setColour (juce::Colour (0xf0e2b96a));
            g.drawText (irName, window.reduced (8.0f * scale, 0.0f),
                        juce::Justification::centred, true);

            // WCAG 2.4.7 Focus Visible: this component fully owns its
            // draw, so the focus indicator is drawn here (same
            // self-contained convention as MasterCropKnob/SpriteToggle).
            if (hasKeyboardFocus (true))
            {
                g.setColour (juce::Colours::white.withAlpha (0.85f));
                g.drawRoundedRectangle (juce::Rectangle<float> (frameLeftPx * scale, frameTopPx * scale,
                                                                (frameRightPx - frameLeftPx) * scale,
                                                                (frameBottomPx - frameTopPx) * scale)
                                            .expanded (2.0f),
                                        6.0f, 1.5f);
            }
        }

        void mouseUp (const juce::MouseEvent& event) override
        {
            if (event.mods.isPopupMenu())
            {
                showContextMenu();
                return;
            }

            if (! event.mouseWasClicked())
                return;

            const auto scale = spriteToLocalScale();
            if (scale <= 0.0f)
                return;

            switch (gestureForPoint (event.position / scale))
            {
                case Gesture::browse:   performBrowse();   break;
                case Gesture::loadFile: performLoadFile(); break;
                case Gesture::none:     break;
            }
        }

        bool keyPressed (const juce::KeyPress& key) override
        {
            if (key.isKeyCode (juce::KeyPress::returnKey) || key.isKeyCode (juce::KeyPress::spaceKey))
            {
                performBrowse();
                return true;
            }

            return false;
        }

        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
        {
            // A button whose accessible VALUE is the loaded IR's name, so
            // AT reads "Impulse Response A, <cabinet>" - the same
            // information the glass window shows sighted users.
            class SlotValueInterface final : public juce::AccessibilityValueInterface
            {
            public:
                explicit SlotValueInterface (IrCartridgeSlot& slotIn) : slot (slotIn) {}
                bool isReadOnly() const override { return true; }
                double getCurrentValue() const override { return 0.0; }
                juce::String getCurrentValueAsString() const override { return slot.irName; }
                void setValue (double) override {}
                void setValueAsString (const juce::String&) override {}
                AccessibleValueRange getRange() const override { return {}; }

            private:
                IrCartridgeSlot& slot;
            };

            return std::make_unique<juce::AccessibilityHandler> (
                *this, juce::AccessibilityRole::button,
                juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press,
                                                        [this] { performBrowse(); }),
                juce::AccessibilityHandler::Interfaces { std::make_unique<SlotValueInterface> (*this) });
        }

    private:
        float spriteToLocalScale() const noexcept
        {
            return sprite.isValid() ? (float) getWidth() / (float) sprite.getWidth() : 0.0f;
        }

        void showContextMenu()
        {
            juce::PopupMenu menu;
            menu.addItem ("Browse impulse response library...", [this] { performBrowse(); });
            menu.addItem ("Load impulse response file...", [this] { performLoadFile(); });
            menu.addItem ("Reset to default", [this] { performResetToDefault(); });

            menu.showMenuAsync (juce::PopupMenu::Options{}.withTargetComponent (this));
        }

        juce::Image sprite;
        juce::String title;
        juce::String irName;
        juce::Font nameFont { juce::FontOptions{}.withHeight (15.0f) };

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (IrCartridgeSlot)
    };
}
