#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"
#include "gui/IrCartridgeSlot.h"
#include "gui/MasterCropKnob.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

// GUI smoke + motion proofs for the wave-3 compositional editor
// (src/PluginEditor.h). juce::ScopedJuceInitialiser_GUI is installed once
// for the whole test binary in tests/TestMain.cpp.
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

    juce::Image snapshotOf (juce::Component& component)
    {
        // SoftwareImageType avoids any dependency on a native graphics
        // context/window - robust on headless CI runners.
        return component.createComponentSnapshot (component.getLocalBounds(), true, 1.0f,
                                                  juce::SoftwareImageType {});
    }

    int changedPixels (const juce::Image& a, const juce::Image& b, juce::Rectangle<int> area, int threshold = 24)
    {
        int changed = 0;

        for (int y = area.getY(); y < area.getBottom(); ++y)
        {
            for (int x = area.getX(); x < area.getRight(); ++x)
            {
                const auto ca = a.getPixelAt (x, y);
                const auto cb = b.getPixelAt (x, y);
                const auto diff = std::abs (ca.getRed() - cb.getRed())
                                 + std::abs (ca.getGreen() - cb.getGreen())
                                 + std::abs (ca.getBlue() - cb.getBlue());
                if (diff > threshold)
                    ++changed;
            }
        }

        return changed;
    }

    // A deliberately "alive-looking" state for the committed preview:
    // varied, non-default knob rotations plus a named cabinet in slot A
    // (pushed through the component's own name surface - the processor
    // path needs a real file on disk, which a headless CI runner does not
    // have).
    void configureLiveLookingState (NaveAudioProcessorEditor& editor)
    {
        struct KnobValue
        {
            const char* title;
            double proportion;
        };

        const KnobValue knobValues[] = {
            { "LoCut", 0.30 }, { "HiCut", 0.70 }, { "IR Blend", 0.45 },
            { "Distance", 0.25 }, { "Mix", 0.85 }, { "Level", 0.50 },
        };

        for (const auto& kv : knobValues)
            if (auto* knob = findChildByTitle<juce::Slider> (editor, kv.title))
                knob->setValue (knob->proportionOfLengthToValue (kv.proportion), juce::dontSendNotification);

        if (auto* slot = findChildByTitle<basilica::gui::IrCartridgeSlot> (editor, "Impulse Response A"))
            slot->setIrName ("modelled_4x12_ceramic_cone");
    }
}

TEST_CASE ("Editor constructs, lays out, and destroys cleanly", "[gui]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    {
        NaveAudioProcessorEditor editor (processor);

        CHECK (editor.getWidth() > 0);
        CHECK (editor.getHeight() > 0);
    }
    // editor destroyed here - JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR
    // asserts at process exit in Debug builds if any tagged instance leaked.
}

TEST_CASE ("Editor snapshot at 100% is non-blank and is written for PR review", "[gui]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    NaveAudioProcessorEditor editor (processor);
    REQUIRE (editor.getWidth() > 0);
    REQUIRE (editor.getHeight() > 0);

    configureLiveLookingState (editor);

    const auto snapshot = snapshotOf (editor);

    REQUIRE (snapshot.isValid());
    CHECK (snapshot.getWidth() == editor.getWidth());
    CHECK (snapshot.getHeight() == editor.getHeight());

    const auto reference = snapshot.getPixelAt (0, 0);
    bool foundDifference = false;

    for (int y = 0; y < snapshot.getHeight() && ! foundDifference; y += juce::jmax (1, snapshot.getHeight() / 20))
        for (int x = 0; x < snapshot.getWidth() && ! foundDifference; x += juce::jmax (1, snapshot.getWidth() / 20))
            if (snapshot.getPixelAt (x, y) != reference)
                foundDifference = true;

    CHECK (foundDifference);

#ifdef NAVE_DOCS_DIR
    // Committed directly for PR review (docs/gui-preview.png) - a TRUE
    // render of the editor tree via the real JUCE draw chain (proof-chain
    // rule), never a hand-mocked composite.
    juce::PNGImageFormat pngFormat;
    const auto outFile = juce::File (NAVE_DOCS_DIR).getChildFile ("gui-preview.png");

    if (auto stream = std::unique_ptr<juce::FileOutputStream> (outFile.createOutputStream()))
    {
        stream->setPosition (0);
        stream->truncate();
        CHECK (pngFormat.writeImageToStream (snapshot, *stream));
    }
    else
    {
        FAIL ("could not open output stream for " << outFile.getFullPathName());
    }
#endif
}

// Proof that the rotating cap crops actually move.
TEST_CASE ("Knob caps visibly rotate at non-default values", "[gui]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    NaveAudioProcessorEditor editor (processor);
    const auto restSnapshot = snapshotOf (editor);
    REQUIRE (restSnapshot.isValid());

    struct ZoomKnob
    {
        const char* title;
        double proportion;
    };

    constexpr ZoomKnob zoomKnobs[] = {
        { "LoCut", 0.02 },
        { "Mix", 0.98 },
        { "IR Blend", 0.15 },
    };

    for (const auto& zk : zoomKnobs)
    {
        auto* knob = findChildByTitle<juce::Slider> (editor, zk.title);
        REQUIRE (knob != nullptr);
        knob->setValue (knob->proportionOfLengthToValue (zk.proportion), juce::dontSendNotification);
    }

    const auto movedSnapshot = snapshotOf (editor);
    REQUIRE (movedSnapshot.isValid());

    for (const auto& zk : zoomKnobs)
    {
        auto* knob = findChildByTitle<juce::Slider> (editor, zk.title);
        REQUIRE (knob != nullptr);

        const auto area = knob->getBounds().expanded (2);
        const auto changed = changedPixels (restSnapshot, movedSnapshot, area);
        const auto total = area.getWidth() * area.getHeight();

        INFO (zk.title << ": " << changed << "/" << total << " px changed between rest and moved pose");
        CHECK (changed > total / 40);
    }
}

// Proof that the D1 windows really are live text surfaces: changing the
// published IR name must visibly change pixels inside the slot's bounds.
TEST_CASE ("The cartridge window shows the loaded IR name as live text", "[gui]")
{
    NaveAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    NaveAudioProcessorEditor editor (processor);

    auto* slot = findChildByTitle<basilica::gui::IrCartridgeSlot> (editor, "Impulse Response A");
    REQUIRE (slot != nullptr);

    slot->setIrName ("Default");
    const auto defaultSnapshot = snapshotOf (editor);

    slot->setIrName ("modelled_8x10_edge");
    const auto namedSnapshot = snapshotOf (editor);

    const auto changed = changedPixels (defaultSnapshot, namedSnapshot, slot->getBounds());
    INFO (changed << " px changed between IR names");
    CHECK (changed > 50);
}
