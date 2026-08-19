#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>

// Nave's own @1x faceplate/control-bay geometry table - lives in its own
// header, rather than as an anonymous-namespace block inside
// PluginEditor.cpp, so tests/gui/EditorLayoutTests.cpp can assert layout
// invariants directly against the SAME numbers PluginEditor.cpp actually
// lays components out with, instead of a second hand-copied set of
// constants that could silently drift out of sync. Mirrors the pattern
// silentium's src/PluginEditorLayout.h (slnt::layout) established for the
// suite's M3 GUI pilot.
//
// This is Nave-specific, art-authored geometry
// (.scaffold/gui-assets/faceplate-nave-v1/layout-manifest.json is the
// canonical bay-rect source for the four control bays below; the header
// band/roundel are auto-derived by render_faceplate.py from canvas size
// alone and were measured directly off the shipped
// faceplate_nave_900x600.png/faceplate_nave_1800x1200.png pixels - see the
// roundelCentre1x/roundelRadius1x docs below - rather than recomputed from
// the render script's own Blender-unit formulas, because the script's
// camera ortho_scale/resolution mapping is not a simple, hand-derivable
// px-per-unit constant).
namespace nave::layout
{
    // juce::Rectangle/Point's constructors are not constexpr (JUCE 8.0.14),
    // so the rects below are plain namespace-scope consts rather than true
    // constexpr - still zero-initialisation-order risk since they only
    // depend on integer literals.
    constexpr int plateWidth1x = 900;
    constexpr int plateHeight1x = 600;

    // Header title-label placement band, vertically centred on the measured
    // roundel centre (see roundelCentre1x below) with the same width/height
    // proportions silentium's own headerBay1x uses on the same 900x600
    // canvas (109px side margins, 71px tall - ample headroom for the 26pt
    // bold title font with no per-pixel dependency on the engraved groove
    // line render_faceplate.py draws underneath it).
    const juce::Rectangle<int> headerBay1x { 109, 64, 682, 71 };

    // Measured directly from faceplate_nave_1800x1200.png (gold/brass-hued
    // pixel cluster nearest the plate's horizontal centre, upper third of
    // the canvas) and cross-checked against faceplate_nave_900x600.png's
    // @1x render (both agree to within rounding) - NOT recomputed from
    // render_faceplate.py's Blender-unit formulas, because the render
    // camera's ortho_scale/resolution mapping (plate_w * 1.1 margin over a
    // 900x600 sensor) is not the same simple UNIT_PER_PX=1/300 constant the
    // script uses to BUILD the mesh, so a naive re-derivation from the
    // script's own math would silently mis-place the roundel by ~20px.
    const juce::Point<int> roundelCentre1x { 450, 100 };
    constexpr int roundelRadius1x = 37;

    // The four engraved bays below, 1:1 with
    // .scaffold/gui-assets/faceplate-nave-v1/layout-manifest.json's
    // cx/cy/w/h rects (centre-based, px @1x) converted to JUCE's top-left
    // convention. NEVER hand-adjust these independently of the manifest -
    // tests/gui/EditorLayoutTests.cpp asserts both representations agree.
    const juce::Rectangle<int> irLoaderBay1x { 100, 145, 700, 100 };
    const juce::Rectangle<int> toneBay1x { 100, 310, 220, 160 };
    const juce::Rectangle<int> characterBay1x { 340, 310, 220, 160 };
    const juce::Rectangle<int> outputBay1x { 580, 310, 220, 160 };

    // Extra strip above the plate art for the preset bar + scale control -
    // interactive text/menus don't fit the plate's own thin engraved header
    // groove at any legible size, so they live in their own band instead,
    // exactly as silentium's editor does.
    constexpr int topStripHeight1x = 32;
    constexpr int topStripGap1x = 6;
    constexpr int scaleButtonWidth1x = 64;

    constexpr int baseEditorWidth = plateWidth1x;
    constexpr int baseEditorHeight = topStripHeight1x + topStripGap1x + plateHeight1x;

    constexpr std::array<float, 3> scaleSteps { 1.0f, 1.5f, 2.0f };

    // Each of the three parameter bays (tone/character/output) holds
    // exactly 2 knobs, side by side, in a single row - unlike silentium's
    // dense 5x2 grid, Nave only has 6 total parameters spread across 3
    // bays, so each bay gets its own simple 2-column split rather than one
    // shared grid.
    constexpr int knobBayCols = 2;
    constexpr int knobLabelHeight1x = 18;
    constexpr int knobDiameter1x = 100;

    // IR loader bay sub-layout: split into two equal halves (slot A left,
    // slot B right), each holding a name label above a Load/Default button
    // row - see PluginEditor.cpp's configureIrSlot().
    constexpr int irSlotLabelHeight1x = 24;
    constexpr int irSlotButtonHeight1x = 32;
    constexpr int irSlotRowGap1x = 8; // between the name label and the button row
    constexpr int irSlotButtonGap1x = 8; // between the Load and Default buttons
    constexpr int irSlotInnerMargin1x = 16;
}
