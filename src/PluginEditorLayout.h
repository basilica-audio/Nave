#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

// Nave's wave-3 COMPOSITIONAL faceplate geometry (campaign 2026-08,
// .scaffold/gui-assets/rollout-2026-07 + DECISIONS.md D1): the plate is
// the accepted EMPTY family plate render (resources/gui/plate_nave.png),
// and every control is composited live from the extracted control-sprite
// library at the coordinates in resources/gui/layout_manifest.json.
//
// This header carries only what is NOT per-control position data (that
// lives in the manifest, the single source of truth - see
// gui/LayoutManifest.h): editor chrome (top strip, stepped scale),
// plate-to-@1x scaling, and the per-SPRITE-FAMILY intrinsic geometry.
//
// SUPERSEDES the M3 filmstrip-generation table this file previously held
// (bay rects for the faceplate-nave-v1 art). The filmstrip components
// (src/gui/FilmstripKnob.h etc.) stay in the tree per the suite's
// "superseded, not deleted" convention, but this editor no longer uses
// them.
namespace nave::layout
{
    constexpr int plateCanvasWidthPx = 1264;
    constexpr int plateCanvasHeightPx = 848;
    constexpr int plateWidth1x = 900;
    constexpr int plateHeight1x = 604;

    constexpr float plateToUnit = (float) plateWidth1x / (float) plateCanvasWidthPx;

    constexpr int topStripHeight1x = 36;
    constexpr int topStripGap1x = 4;
    constexpr int scaleButtonWidth1x = 64;

    constexpr int baseEditorWidth = plateWidth1x;
    constexpr int baseEditorHeight = topStripHeight1x + topStripGap1x + plateHeight1x;

    constexpr std::array<float, 3> scaleSteps { 1.0f, 1.5f, 2.0f };

    // ==================== sprite-family intrinsic geometry ====================
    // (measured once against the sprite PNGs - sprite-library extraction
    // wave 2026-08-27.)

    // sprite_knob_brass.png (148x148, master-05 row-1 far-right knob).
    constexpr float knobAnchorX = 75.5f;
    constexpr float knobAnchorY = 70.0f;
    constexpr float knobCapRadius = 34.0f;

    // sprite_ir_slot_{a,b}.png (522x207, D1 cartridge slots from the nave
    // wave-1 base render): the manifest positions the CANVAS CENTRE; the
    // window/button hit-zone geometry lives with the component
    // (gui/IrCartridgeSlot.h's sprite-space constants).
    constexpr float slotAnchorX = 261.0f;
    constexpr float slotAnchorY = 103.5f;
    constexpr float slotSpriteWidthPx = 522.0f;
    constexpr float slotSpriteHeightPx = 207.0f;

    constexpr float knobSweepDeg = 270.0f;

    // ==================== engraved lettering ====================
    constexpr float labelBoxWidthPlatePx = 150.0f;
    constexpr float labelBoxHeightPlatePx = 26.0f;

    // The IR-name font height inside a slot's smoked-glass window, in
    // sprite px of the 522x207 cartridge sprite (IrCartridgeSlot scales it
    // with its own bounds via the editor's applyScaleStep()).
    constexpr float slotNameFontSpritePx = 30.0f;

    // Issue #42's preset-IR notice strip, @1x px (the old editor's
    // geometry, carried over - full-width strip near the plate's bottom).
    const juce::Rectangle<int> irNoticeStrip1x { 20, 560, 860, 30 };
}
