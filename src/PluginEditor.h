#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

#include "gui/BasilicaLookAndFeel.h"
#include "gui/IrBrowserPanel.h"
#include "gui/IrCartridgeSlot.h"
#include "gui/LayoutManifest.h"
#include "gui/MasterCropKnob.h"
#include "gui/PlateTypography.h"
#include "ir/FactoryIrLibrary.h"
#include "presets/PresetBar.h"

class NaveAudioProcessor;

namespace nave
{
    // Nave's bundled IR library (issue #33) as embedded bytes, in the exact
    // list the browser's "Install Library" button writes to disk.
    //
    // Declared here rather than kept file-local in PluginEditor.cpp so that
    // tests can assert the *shipped* list against the verified files in
    // resources/irs/ - the one thing that could silently go wrong with an
    // embedded copy is drifting away from the audio that verify_irs.py and
    // the committed SHA-256 manifest actually vouch for. The definition
    // lives in PluginEditor.cpp, which is one of the two translation units
    // permitted to include BinaryData.h (see CMakeLists.txt).
    const std::vector<basilica::ir::FactoryIrAsset>& factoryIrAssets();
}

// Wave-3 COMPOSITIONAL photoreal editor (campaign 2026-08, supersedes the
// M3 filmstrip editor - FilmstripKnob and the faceplate-nave-v1 assets
// stay in the tree per the suite's "superseded, not deleted" convention):
// the accepted EMPTY family plate render (resources/gui/plate_nave.png) is
// the sole baked background, and every control is composited live from the
// extracted control-sprite library at the coordinates in
// resources/gui/layout_manifest.json (single source of truth - see
// gui/LayoutManifest.h). Draw order:
//
//   1. plate render (paint())
//   2. static knob sprites (paint(), under the children)
//   3. engraved lettering - PlateTypography, gilded gold on dark basalt
//   4. rotating cap crops - one MasterCropKnob child per knob
//   5. D1 IR cartridge slots - two IrCartridgeSlot children drawing their
//      own sprite + the loaded IR's warm-gold name, wired to the REAL IR
//      A/B loaders (browser overlay, direct file chooser, default reset -
//      see IrCartridgeSlot.h's operability contract)
//   6. the IR browser overlay + issue #42's preset-IR notice, carried
//      over unchanged from the M3 editor
//
// Nave-specific control set (rollout-2026-07/nave/control-inventory.md):
// 6 knobs (4+2 rows), 0 toggles (no host-visible bypass parameter),
// 0 meters (no metering DSP - no dead decoration), 2 D1 slots.
//
// Window scaling is STEPPED (100/150/200%, UA-style corner control,
// persisted as a plain property on the APVTS state tree).
class NaveAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit NaveAudioProcessorEditor (NaveAudioProcessor& processorToEdit);
    ~NaveAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // The parsed layout manifest - exposed read-only so tests assert
    // layout invariants against the exact data this editor composites
    // from (tests/gui/EditorLayoutTests.cpp).
    const basilica::gui::LayoutManifest& layoutManifest() const noexcept { return manifest; }

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    struct Knob
    {
        const basilica::gui::ManifestControl* entry = nullptr;
        std::unique_ptr<basilica::gui::MasterCropKnob> slider;
        std::unique_ptr<SliderAttachment> attachment;
    };

    // Which of the two independent IR slots (plain ValueTree properties,
    // not APVTS parameters - see ParamIDs) a cartridge instance controls.
    enum class IrSlotId
    {
        A,
        B
    };

    struct Slot
    {
        const basilica::gui::ManifestControl* entry = nullptr;
        IrSlotId id = IrSlotId::A;
        juce::String label;
        std::unique_ptr<basilica::gui::IrCartridgeSlot> component;
        std::unique_ptr<juce::FileChooser> activeFileChooser;
    };

    juce::Image spriteImageFor (const juce::String& spriteKey) const;
    void buildControlsFromManifest();
    void configureSlotCallbacks (Slot& slot);
    void refreshSlotName (Slot& slot);
    void chooseImpulseResponseForSlot (Slot& slot);
    void openIrBrowserForSlot (Slot& slot);
    void showPresetIrNotice (const juce::String& message);
    Slot* slotFor (IrSlotId id) noexcept;
    void applyScaleStep (int newStepIndex);
    void cycleScale();
    void drawStaticSprites (juce::Graphics& g) const;
    void drawPlateLettering (juce::Graphics& g) const;

    float plateScale() const noexcept;
    juce::Point<float> plateOrigin() const noexcept;

    NaveAudioProcessor& audioProcessor;

    // Installed on `this` so the IR browser overlay + preset bar keep the
    // suite styling (the compositional plate itself needs no LookAndFeel).
    basilica::gui::BasilicaLookAndFeel lookAndFeel;

    basilica::gui::LayoutManifest manifest;

    juce::Image plateImage;
    juce::Image knobSprite, slotSpriteA, slotSpriteB;

    basilica::presets::PresetBar presetBar;
    juce::TextButton scaleButton;
    int scaleStepIndex = 0; // 0 = 100%, 1 = 150%, 2 = 200%

    std::vector<Knob> knobs;
    std::vector<std::unique_ptr<Slot>> slots;

    // The IR browser overlay (issue #1). One shared instance for both
    // slots, retargeted per open; ADDED last in the constructor body so it
    // covers every other child when visible.
    basilica::gui::IrBrowserPanel irBrowserPanel;
    IrSlotId irBrowserTargetSlot = IrSlotId::A;

    juce::Label presetIrNoticeLabel;

    basilica::gui::PlateTypography typography;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NaveAudioProcessorEditor)
};
