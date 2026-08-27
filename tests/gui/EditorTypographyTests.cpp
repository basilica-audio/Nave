#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"
#include "gui/PlateTypography.h"

#include <BinaryData.h>

#include <catch2/catch_test_macros.hpp>

#include <cmath>

// Typography-pass proof for the wave-3 compositional editor: no lettering
// is baked into the empty family plate or the control sprites (the wave-2
// plates were explicitly gated on "no text anywhere" - see
// rollout-2026-07/nave/out/VERDICT.md), so every engraved label is a
// live JUCE text layer (src/gui/PlateTypography.h, gilded gold on the dark
// basalt). Proofs: (1) each manifest label box is measurably brighter in
// the editor snapshot than the raw plate render at the same position;
// (2) a flat-ground unit render of the shared glyph draw path. Floors are
// deliberately cross-platform-loose - the Windows glyph rasterizer renders
// visibly thinner coverage than macOS for the same face/height (suite
// lesson, aureate/apotheosis typography wave).
namespace
{
    float fractionBrighterThan (const juce::Image& image, juce::Rectangle<int> area, int threshold)
    {
        int hits = 0, total = 0;

        for (int y = area.getY(); y < area.getBottom(); ++y)
        {
            for (int x = area.getX(); x < area.getRight(); ++x)
            {
                const auto c = image.getPixelAt (x, y);
                const auto lum = (int) std::lround (0.299f * c.getRed() + 0.587f * c.getGreen() + 0.114f * c.getBlue());

                ++total;
                if (lum > threshold)
                    ++hits;
            }
        }

        return total > 0 ? (float) hits / (float) total : 0.0f;
    }
}

TEST_CASE ("Engraved labels brighten the dark plate where the raw render is clean", "[gui][typography]")
{
    using namespace nave::layout;

    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    NaveAudioProcessorEditor editor (processor);
    REQUIRE (editor.getWidth() > 0);

    const auto snapshot = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f,
                                                          juce::SoftwareImageType {});
    REQUIRE (snapshot.isValid());

    const auto plate = juce::ImageCache::getFromMemory (BinaryData::plate_nave_png,
                                                        BinaryData::plate_nave_pngSize);
    REQUIRE (plate.isValid());

    const auto& manifest = editor.layoutManifest();
    REQUIRE (manifest.isValid());

    int labelledControls = 0;

    for (const auto& control : manifest.controls)
    {
        if (control.label.isEmpty() || control.labelCy <= 0.0f)
            continue;

        ++labelledControls;

        // Tight text-core box (the full label box may graze a sprite's
        // feathered basalt edge, whose brass could contaminate the
        // plate-side reading).
        const juce::Rectangle<float> corePlatePx (control.cx - labelBoxWidthPlatePx * 0.4f,
                                                  control.labelCy - labelBoxHeightPlatePx * 0.35f,
                                                  labelBoxWidthPlatePx * 0.8f,
                                                  labelBoxHeightPlatePx * 0.7f);

        // Snapshot space: plate px scaled by plateToUnit, offset by the
        // top chrome strip.
        const auto toSnapshot = juce::Rectangle<float> (
            corePlatePx.getX() * plateToUnit,
            (float) (topStripHeight1x + topStripGap1x) + corePlatePx.getY() * plateToUnit,
            corePlatePx.getWidth() * plateToUnit, corePlatePx.getHeight() * plateToUnit)
                                    .getSmallestIntegerContainer();

        const auto toPlate = corePlatePx.getSmallestIntegerContainer();

        const auto snapshotBright = fractionBrighterThan (snapshot, toSnapshot, 110);
        const auto plateBright = fractionBrighterThan (plate, toPlate, 110);

        INFO ("label \"" << control.label.toStdString() << "\": snapshot " << snapshotBright
                          << " vs raw plate " << plateBright);
        CHECK (snapshotBright > plateBright + 0.02f);
    }

    CHECK (labelledControls == 6); // the 6 knobs (the cartridge slots carry baked A/B engravings + live window text instead)
}

TEST_CASE ("PlateTypography renders glyphs and its offset pass on a flat ground", "[gui][typography]")
{
    // Flat-ground unit proof for the ONE shared draw path every engraved
    // label goes through.
    basilica::gui::PlateTypography typography (BinaryData::EBGaramondRegular_ttf,
                                               (int) BinaryData::EBGaramondRegular_ttfSize,
                                               BinaryData::EBGaramondSemiBold_ttf,
                                               (int) BinaryData::EBGaramondSemiBold_ttfSize);

    const juce::Colour ground (0xff141116); // near-black basalt, luminance ~19

    juce::Image canvas (juce::Image::RGB, 160, 24, true);
    {
        juce::Graphics g (canvas);
        g.fillAll (ground);

        const basilica::gui::EngravedTextStyle style {
            juce::Colour (0xf0d6ad5e), juce::Colour (0x8c000000), 13.0f, 0.16f, true
        };

        typography.drawEngraved (g, "DISTANCE", canvas.getBounds().toFloat(), 1.0f, style);
    }

    int goldPixels = 0, nonGroundPixels = 0;
    const auto groundLum = 0.299f * ground.getRed() + 0.587f * ground.getGreen() + 0.114f * ground.getBlue();

    for (int y = 0; y < canvas.getHeight(); ++y)
    {
        for (int x = 0; x < canvas.getWidth(); ++x)
        {
            const auto c = canvas.getPixelAt (x, y);
            const auto lum = 0.299f * c.getRed() + 0.587f * c.getGreen() + 0.114f * c.getBlue();

            if (lum > 120.0f)
                ++goldPixels;

            if (std::abs (lum - groundLum) > 8.0f)
                ++nonGroundPixels;
        }
    }

    // Cross-platform-loose floors (see this file's top-of-file docs).
    CHECK (goldPixels > 30);
    CHECK (nonGroundPixels > 150);
}
