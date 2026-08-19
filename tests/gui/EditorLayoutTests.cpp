#include "PluginEditorLayout.h"

#include <catch2/catch_test_macros.hpp>

// Layout-invariant tests: assert directly against the same nave::layout
// constants PluginEditor.cpp lays components out with (see
// PluginEditorLayout.h), so this test and the actual layout can never
// silently drift apart - mirrors silentium's M3 pilot
// (tests/gui/EditorLayoutTests.cpp there).
TEST_CASE ("IR loader bay starts at or below the header bay's bottom edge", "[gui][layout]")
{
    using namespace nave::layout;

    CHECK (irLoaderBay1x.getY() >= headerBay1x.getBottom());
}

TEST_CASE ("The three parameter bays start at or below the IR loader bay's bottom edge", "[gui][layout]")
{
    using namespace nave::layout;

    CHECK (toneBay1x.getY() >= irLoaderBay1x.getBottom());
    CHECK (characterBay1x.getY() >= irLoaderBay1x.getBottom());
    CHECK (outputBay1x.getY() >= irLoaderBay1x.getBottom());
}

TEST_CASE ("The three parameter bays sit in a single non-overlapping row", "[gui][layout]")
{
    using namespace nave::layout;

    CHECK (toneBay1x.getRight() <= characterBay1x.getX());
    CHECK (characterBay1x.getRight() <= outputBay1x.getX());

    // Same top edge and height, i.e. genuinely one row, not staggered.
    CHECK (toneBay1x.getY() == characterBay1x.getY());
    CHECK (characterBay1x.getY() == outputBay1x.getY());
    CHECK (toneBay1x.getHeight() == characterBay1x.getHeight());
    CHECK (characterBay1x.getHeight() == outputBay1x.getHeight());
}

TEST_CASE ("Header bay starts at or below the plate's top edge and above the IR loader bay", "[gui][layout]")
{
    using namespace nave::layout;

    CHECK (headerBay1x.getY() >= 0);
    CHECK (headerBay1x.getBottom() <= irLoaderBay1x.getY());
}

TEST_CASE ("Each parameter bay is wide/tall enough for two knob columns, each with a label plus a full-diameter knob", "[gui][layout]")
{
    using namespace nave::layout;

    for (const auto& bay : { toneBay1x, characterBay1x, outputBay1x })
    {
        const auto cellW = bay.getWidth() / knobBayCols;
        CHECK (cellW >= knobDiameter1x);
        CHECK (bay.getHeight() - knobLabelHeight1x >= knobDiameter1x);
    }
}

TEST_CASE ("IR loader bay is tall enough for a name label plus a button row with no overlap", "[gui][layout]")
{
    using namespace nave::layout;

    const auto contentHeight = irSlotLabelHeight1x + irSlotRowGap1x + irSlotButtonHeight1x;
    CHECK (irLoaderBay1x.getHeight() >= contentHeight);
}

TEST_CASE ("IR loader bay is wide enough for two slots with inner margins and a button gap, with no overlap", "[gui][layout]")
{
    using namespace nave::layout;

    const auto halfW = irLoaderBay1x.getWidth() / 2;
    const auto slotContentWidth = halfW - 2 * irSlotInnerMargin1x;
    CHECK (slotContentWidth > irSlotButtonGap1x);
}

TEST_CASE ("Every laid-out bay stays within the plate's own canvas bounds", "[gui][layout]")
{
    using namespace nave::layout;

    const juce::Rectangle<int> plateCanvas { 0, 0, plateWidth1x, plateHeight1x };

    for (const auto& bay : { headerBay1x, irLoaderBay1x, toneBay1x, characterBay1x, outputBay1x })
        CHECK (plateCanvas.contains (bay));
}

TEST_CASE ("Every laid-out bay matches the faceplate-nave-v1 layout-manifest.json centre-based rects", "[gui][layout]")
{
    // Manifest rects (.scaffold/gui-assets/faceplate-nave-v1/layout-manifest.json,
    // "main @ 2026-07-16" param source) are centre-based (cx, cy, w, h) - this
    // test converts nave::layout's top-left rects back to that representation
    // and checks they agree exactly, so a future manifest update can't
    // silently drift out of sync with what the editor actually renders.
    using namespace nave::layout;

    struct ManifestRect
    {
        int cx, cy, w, h;
    };

    const auto matchesManifest = [] (juce::Rectangle<int> rect, ManifestRect manifest)
    {
        return rect.getCentreX() == manifest.cx
            && rect.getCentreY() == manifest.cy
            && rect.getWidth() == manifest.w
            && rect.getHeight() == manifest.h;
    };

    CHECK (matchesManifest (irLoaderBay1x, { 450, 195, 700, 100 }));
    CHECK (matchesManifest (toneBay1x, { 210, 390, 220, 160 }));
    CHECK (matchesManifest (characterBay1x, { 450, 390, 220, 160 }));
    CHECK (matchesManifest (outputBay1x, { 690, 390, 220, 160 }));
}
