#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"

#include <BinaryData.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

// Wave-3 compositional-layout invariants, asserted against the SAME parsed
// manifest the editor composites from (PluginEditor::layoutManifest() /
// gui/LayoutManifest.h) - never a second hand-maintained coordinate list.
// The expected control census comes from the rollout control inventory
// (.scaffold/gui-assets/rollout-2026-07/nave/control-inventory.md +
// DECISIONS.md D1): 6 knobs (4+2 rows), 2 D1 cartridge slots, 0 toggles,
// 0 meters.
namespace
{
    basilica::gui::LayoutManifest parseManifest()
    {
        return basilica::gui::LayoutManifest::parse (BinaryData::layout_manifest_json,
                                                     BinaryData::layout_manifest_jsonSize);
    }

    constexpr float fieldLeft = 85.0f, fieldRight = 1162.0f;
    constexpr float fieldTop = 85.0f, fieldBottom = 768.0f;

    const juce::Rectangle<float> dividerKeepOut (500.0f, 444.0f, 250.0f, 20.0f);
    const juce::Rectangle<float> ventLeftKeepOut (150.0f, 488.0f, 142.0f, 195.0f);
    const juce::Rectangle<float> ventRightKeepOut (963.0f, 488.0f, 142.0f, 195.0f);

    float capRadiusPlatePx (const basilica::gui::ManifestControl& control)
    {
        return nave::layout::knobCapRadius * control.scale;
    }

    // A cartridge slot is a wide rectangle - its hardware extent (the
    // brass frame, IrCartridgeSlot.h's sprite-space frame box) for
    // keep-out purposes.
    juce::Rectangle<float> slotHardwareBox (const basilica::gui::ManifestControl& slot)
    {
        using S = basilica::gui::IrCartridgeSlot;
        const auto w = (S::frameRightPx - S::frameLeftPx) * slot.scale;
        const auto h = (S::frameBottomPx - S::frameTopPx) * slot.scale;
        return { slot.cx - w * 0.5f, slot.cy - h * 0.5f, w, h };
    }
}

TEST_CASE ("Manifest parses and matches the rollout control inventory census", "[gui][layout]")
{
    const auto manifest = parseManifest();

    REQUIRE (manifest.isValid());
    CHECK (manifest.plateWidthPx == nave::layout::plateCanvasWidthPx);
    CHECK (manifest.plateHeightPx == nave::layout::plateCanvasHeightPx);

    CHECK (manifest.ofKind ("knob").size() == 6);
    CHECK (manifest.ofKind ("slot").size() == 2);   // the D1 cartridge pair
    CHECK (manifest.ofKind ("toggle").empty());     // no host-visible bypass param
    CHECK (manifest.ofKind ("meter").empty());      // no metering DSP - no dead decoration
    CHECK (manifest.controls.size() == 8);
}

TEST_CASE ("Every knob manifest id resolves to a real APVTS float parameter", "[gui][layout]")
{
    const auto manifest = parseManifest();
    REQUIRE (manifest.isValid());

    NaveAudioProcessor processor;

    for (const auto* knob : manifest.ofKind ("knob"))
    {
        auto* parameter = processor.apvts.getParameter (knob->id);
        INFO ("manifest id \"" << knob->id.toStdString() << "\"");
        REQUIRE (parameter != nullptr);
        CHECK (dynamic_cast<juce::AudioParameterFloat*> (parameter) != nullptr);
    }
}

TEST_CASE ("Knob rows follow the 4+2 family split with a uniform row-1 rhythm", "[gui][layout]")
{
    const auto manifest = parseManifest();
    REQUIRE (manifest.isValid());

    std::map<float, std::vector<float>> rows;

    for (const auto* knob : manifest.ofKind ("knob"))
        rows[knob->cy].push_back (knob->cx);

    REQUIRE (rows.size() == 2);

    auto it = rows.begin();
    auto& row1 = it->second;
    auto& row2 = std::next (it)->second;

    CHECK (row1.size() == 4);
    CHECK (row2.size() == 2);

    std::sort (row1.begin(), row1.end());
    for (size_t i = 2; i < row1.size(); ++i)
        CHECK (std::abs ((row1[i] - row1[i - 1]) - (row1[1] - row1[0])) < 1.0f);

    // The output pair sits centred on the plate's vertical centre line.
    std::sort (row2.begin(), row2.end());
    const auto centre = (float) nave::layout::plateCanvasWidthPx * 0.5f;
    CHECK (std::abs ((centre - row2[0]) - (row2[1] - centre)) < 1.0f);
}

TEST_CASE ("The cartridge slots sit side by side, mirrored about the centre line", "[gui][layout]")
{
    const auto manifest = parseManifest();
    REQUIRE (manifest.isValid());

    const auto slots = manifest.ofKind ("slot");
    REQUIRE (slots.size() == 2);

    CHECK (slots[0]->cy == slots[1]->cy);
    CHECK (slots[0]->scale == slots[1]->scale);

    const auto centre = (float) nave::layout::plateCanvasWidthPx * 0.5f;
    CHECK (std::abs ((centre - slots[0]->cx) - (slots[1]->cx - centre)) < 1.0f);

    // A is left of B (the baked A/B engravings must match reading order).
    const auto* slotA = manifest.findById ("irSlotA");
    const auto* slotB = manifest.findById ("irSlotB");
    REQUIRE (slotA != nullptr);
    REQUIRE (slotB != nullptr);
    CHECK (slotA->cx < slotB->cx);
}

TEST_CASE ("Every control stays inside the pinstripe field and off the baked plate art", "[gui][layout]")
{
    const auto manifest = parseManifest();
    REQUIRE (manifest.isValid());

    for (const auto& control : manifest.controls)
    {
        INFO ("control \"" << control.id.toStdString() << "\"");

        if (control.kind == "slot")
        {
            const auto box = slotHardwareBox (control);
            CHECK (box.getX() >= fieldLeft);
            CHECK (box.getRight() <= fieldRight);
            CHECK (box.getY() >= fieldTop);
            CHECK (box.getBottom() <= fieldBottom);
            CHECK_FALSE (box.intersects (dividerKeepOut));
            CHECK_FALSE (box.intersects (ventLeftKeepOut));
            CHECK_FALSE (box.intersects (ventRightKeepOut));
            continue;
        }

        const auto r = capRadiusPlatePx (control);

        CHECK (control.cx - r >= fieldLeft);
        CHECK (control.cx + r <= fieldRight);
        CHECK (control.cy - r >= fieldTop);
        CHECK (control.cy + r <= fieldBottom);

        const juce::Rectangle<float> capBox (control.cx - r, control.cy - r, 2.0f * r, 2.0f * r);
        CHECK_FALSE (capBox.intersects (dividerKeepOut));
        CHECK_FALSE (capBox.intersects (ventLeftKeepOut));
        CHECK_FALSE (capBox.intersects (ventRightKeepOut));

        if (control.labelCy > 0.0f)
        {
            using namespace nave::layout;
            const juce::Rectangle<float> labelBox (control.cx - labelBoxWidthPlatePx * 0.5f,
                                                   control.labelCy - labelBoxHeightPlatePx * 0.5f,
                                                   labelBoxWidthPlatePx, labelBoxHeightPlatePx);

            CHECK (labelBox.getY() >= fieldTop);
            CHECK (labelBox.getBottom() <= fieldBottom);
            CHECK (labelBox.getY() >= control.cy + r - 1.0f);
        }
    }
}

TEST_CASE ("No two composited elements overlap", "[gui][layout]")
{
    const auto manifest = parseManifest();
    REQUIRE (manifest.isValid());

    for (size_t a = 0; a < manifest.controls.size(); ++a)
    {
        for (size_t b = a + 1; b < manifest.controls.size(); ++b)
        {
            const auto& ca = manifest.controls[a];
            const auto& cb = manifest.controls[b];

            INFO (ca.id.toStdString() << " vs " << cb.id.toStdString());

            if (ca.kind == "slot" || cb.kind == "slot")
            {
                const auto& slot = ca.kind == "slot" ? ca : cb;
                const auto& other = ca.kind == "slot" ? cb : ca;

                if (other.kind == "slot")
                {
                    CHECK_FALSE (slotHardwareBox (slot).intersects (slotHardwareBox (other)));
                }
                else
                {
                    const auto r = capRadiusPlatePx (other);
                    const juce::Rectangle<float> otherBox (other.cx - r, other.cy - r, 2.0f * r, 2.0f * r);
                    CHECK_FALSE (slotHardwareBox (slot).intersects (otherBox));
                }
                continue;
            }

            const auto minGap = capRadiusPlatePx (ca) + capRadiusPlatePx (cb);
            const auto dx = ca.cx - cb.cx;
            const auto dy = ca.cy - cb.cy;

            CHECK (dx * dx + dy * dy >= minGap * minGap);
        }
    }
}

TEST_CASE ("Editor base size derives from the plate geometry", "[gui][layout]")
{
    using namespace nave::layout;

    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    NaveAudioProcessorEditor editor (processor);

    CHECK (editor.getWidth() == baseEditorWidth);
    CHECK (editor.getHeight() == baseEditorHeight);
    CHECK (editor.layoutManifest().isValid());
}
