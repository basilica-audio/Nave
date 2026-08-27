#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/CabConvolutionEngine.h"
#include "ir/BundledIrSource.h"
#include "ir/IrContentIndex.h"
#include "presets/IrReference.h"
#include "presets/PresetManager.h"

#include <functional>

// Nave: a cabinet impulse-response (IR) loader for reamping guitar/bass DI
// tracks. Signal flow lives in CabConvolutionEngine (src/dsp) so it stays
// unit-testable independent of this AudioProcessor; this class is just
// APVTS + host plumbing + IR file I/O around it.
// The four v0.3.0 parameters that change convolver CONTENT (align mode, gain
// mode, and the two per-slot min-phase switches) cannot be applied from
// processBlock(): each re-runs FFT-scale analysis and reloads the stock
// convolution engines. But APVTS parameter listeners fire on whichever thread
// set the value, which for host automation is the audio thread.
//
// AsyncUpdater is the bridge: parameterChanged() only calls
// triggerAsyncUpdate() (documented real-time safe, and it coalesces), and the
// actual reconfiguration happens later on the message thread in
// handleAsyncUpdate(). The remaining parameters stay on the per-block polling
// path, which is cheaper and has no ordering requirements.
class NaveAudioProcessor final : public juce::AudioProcessor,
                                  private juce::AudioProcessorValueTreeState::Listener,
                                  private juce::AsyncUpdater
{
public:
    NaveAudioProcessor();
    ~NaveAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    //==============================================================================
    // Loads a new impulse response from an audio file (WAV/AIFF, or any other
    // format juce::AudioFormatManager::registerBasicFormats() understands).
    // MUST be called off the audio thread (e.g. the message thread, from the
    // editor's file-chooser callback, or from a test) - this performs
    // blocking file I/O. On success, the file's absolute path is stored as a
    // property on apvts.state so it round-trips through
    // getStateInformation()/setStateInformation() alongside the regular
    // parameters. Returns false (leaving the current IR unchanged) if the
    // file cannot be read as audio.
    bool loadImpulseResponseFromFile (const juce::File& irFile);

    // Reverts to the plugin's default unit-impulse (delta) IR and clears the
    // stored IR file path. Same off-audio-thread contract as
    // loadImpulseResponseFromFile().
    void loadDefaultImpulseResponse();

    // The absolute path of the currently loaded IR file, or an empty string
    // if the default (no user IR) is active. Safe to call from the message
    // thread (editor display) at any time.
    juce::String getCurrentIrFilePath() const;

    // Same three operations as above, for the secondary IR slot (IR B) used
    // by the IR Blend parameter.
    bool loadImpulseResponseFromFileB (const juce::File& irFile);
    void loadDefaultImpulseResponseB();
    juce::String getCurrentIrFilePathB() const;

    // The RAW impulse response the convolution engine currently holds for
    // slot `slotIndex` (0 = A, 1 = B) - pre-alignment, pre-min-phase,
    // pre-normalisation, i.e. exactly the samples getStateInformation()
    // embeds. Message thread only.
    //
    // A pass-through of CabConvolutionEngine's own public accessor, exposed
    // here so a caller can ask what the CONVOLVER holds rather than what a
    // path property says it should. Those are different questions:
    // getCurrentIrFilePath() would still name a file if the audio behind it
    // had never reached the DSP, which is the difference between proving a
    // preset's IR reference selected an IR and proving a JSON field
    // round-tripped (see tests/PresetIrReferenceTests.cpp).
    const juce::AudioBuffer<float>& getLoadedImpulseResponse (int slotIndex) const noexcept;
    double getLoadedImpulseResponseSampleRate (int slotIndex) const noexcept;

    // Installs this processor's issue-#42 IR-reference hooks onto `manager`
    // (see PresetManagerConfig::captureExtraFields/applyExtraFields). Done
    // for the processor's own `presetManager` at construction; also public so
    // a test can drive an isolated manager - one pointed at a scratch preset
    // directory instead of the user's real one - through the exact production
    // code path rather than a re-implementation of it.
    void installPresetIrCallbacks (basilica::presets::PresetManager& manager);

    juce::AudioProcessorValueTreeState apvts;

    // M2 preset system (.scaffold/specs/preset-system-m2.md,
    // src/presets/PresetManager.h). Constructed after apvts (its
    // constructor registers APVTS parameter listeners) and public so
    // NaveAudioProcessorEditor's PresetBar can talk to it directly - the
    // same "processor owns it, editor references it" pattern apvts itself
    // already uses.
    basilica::presets::PresetManager presetManager;

    //==============================================================================
    // Preset -> IR references (issue #42, src/presets/IrReference.h).

    // The notice raised by the most recent preset load, or an empty string
    // when that load had nothing to report. Non-empty means the preset named
    // an IR that is not in the user's library: its PARAMETERS were still
    // applied and the IR slots were left exactly as they were - a preset never
    // fails to open over a missing IR, and a missing IR is never quietly
    // replaced by a different one. Message thread only.
    juce::String getPresetIrNotice() const { return presetIrNotice; }

    // Called on the message thread, immediately after a preset load that
    // raised a notice, with the same text getPresetIrNotice() returns. The
    // editor sets this to show the notice non-modally and clears it in its
    // destructor; a headless/no-editor instance simply leaves it null and the
    // text is still readable via getPresetIrNotice().
    std::function<void (const juce::String&)> onPresetIrNotice;

    // Folders searched (in order) when resolving a preset's IR reference: the
    // library folder the IR browser is pointed at, then the out-of-the-box
    // default one. Exposed for tests and for the editor, which re-points the
    // index when the user changes their library folder.
    std::vector<juce::File> getIrSearchRoots() const;
    void refreshIrSearchRoots();

    //==============================================================================
    // Resolving one slot's reference (issue #45).

    // WHERE a referenced IR came from. This enumeration is TOTAL: every
    // digest a preset can carry lands on exactly one of these, including the
    // ones that name nothing and the ones that name something nobody has.
    // There is no "undefined" outcome and no path that leaves the caller
    // guessing what happened to a slot.
    enum class IrReferenceSource
    {
        // The preset carries no digest for this slot. Not an error, and by
        // far the commonest case - every preset written before #42, and every
        // preset saved with no user IR loaded.
        notReferenced,

        // The slot ALREADY holds exactly those bytes. The convolver is left
        // alone: reloading it would be a glitch in exchange for nothing.
        alreadyLoaded,

        // A file under the user's IR search roots hashes to the digest. This
        // is checked FIRST - see the precedence note in
        // src/ir/BundledIrSource.h.
        library,

        // Nave's own embedded copy, written out to
        // IrLibrary::bundledCacheDirectory() and then loaded through the same
        // file path a browser selection uses. Only reachable for the nine
        // bundled digests.
        bundled,

        // Nobody has these bytes - not the library, not the bundle - or the
        // bundled copy could not be written to disk. The slot is LEFT AS IT
        // IS: whatever was playing keeps playing, nothing is substituted, and
        // the caller raises a notice. This is the degradation path, and it is
        // audibly safe by construction because it performs no audio operation
        // at all.
        notFound
    };

    struct ResolvedIrReference
    {
        IrReferenceSource source = IrReferenceSource::notFound;

        // A real, existing file when `source` is library, bundled or
        // alreadyLoaded; a default juce::File otherwise. Never a path to
        // something that is not there.
        juce::File file;
    };

    // Answers "where do I get the bytes with this digest". Refreshes the
    // search roots itself, so it is safe to call standalone. Blocking file
    // I/O (directory scan, hashing, possibly one small write): message thread
    // only, never processBlock().
    //
    // `currentIrPath` is the path the slot currently holds, used only for the
    // alreadyLoaded short-circuit; pass an empty string to skip it.
    ResolvedIrReference resolveIrReference (const juce::String& contentHash,
                                            const juce::String& currentIrPath);

    // Where the embedded IRs are written out to when a reference resolves
    // against the bundle. Defaults to IrLibrary::bundledCacheDirectory();
    // the setter exists so tests never write into the real user location
    // (the same role PresetManagerConfig::userPresetsDirectoryOverrideForTests
    // plays for presets). Setting an empty File restores the default.
    juce::File getBundledIrCacheDirectory() const;
    void setBundledIrCacheDirectoryForTests (const juce::File& folder);

private:
    void parameterChanged (const juce::String& parameterId, float newValue) override;
    void handleAsyncUpdate() override;

    // Pushes the audio-thread-safe v0.3.0 parameters into the engine. Called
    // every block, like the v0.1/v0.2 parameters.
    void applyAudioThreadParameters();

    // Reads the four message-thread parameters out of the APVTS and pushes
    // them into the engine. Message thread only.
    void reconfigureEngineFromParameters();

    // Writes the current raw IR buffers into apvts.state as embedded audio
    // blobs, and stamps the schema version. Called from getStateInformation().
    void embedImpulseResponsesIntoState();

    // Restores both slots on load, in the documented precedence order:
    // embedded audio first (authoritative), then the stored path, then the
    // default delta IR.
    void restoreImpulseResponsesFromState();

    // PresetManagerConfig::captureExtraFields: records the SHA-256 (plus a
    // display name, for messages only) of whatever is loaded in each IR slot
    // onto a preset being saved. Writes nothing when no IR is loaded, so a
    // preset saved from the default delta IR is byte-identical to what the
    // pre-#42 saver produced.
    void capturePresetIrReferences (juce::DynamicObject& presetObject);

    // PresetManagerConfig::applyExtraFields: the whole of decision D2. For
    // each referenced slot: if the loaded IR already IS those bytes, do
    // nothing; else look the bytes up in irContentIndex and load the file
    // that matches; else - and this is the case that matters - leave the slot
    // untouched and add the expected IR's name to the notice. Never
    // substitutes, never refuses the preset.
    void applyPresetIrReferences (const juce::var& presetObject);

    // The bundled library's display name for a digest, or an empty string
    // when the digest is not one of Nave's own IRs. Used to make a miss
    // actionable and to name an IR whose preset recorded no name.
    juce::String bundledDisplayNameForContentHash (const juce::String& contentHash) const;

    basilica::ir::IrContentIndex irContentIndex;
    juce::File bundledIrCacheDirectoryOverride; // empty = the real location
    juce::String presetIrNotice;

    CabConvolutionEngine engine;

    // Raw atomic pointers into the APVTS-managed parameter values, resolved
    // once at construction time so processBlock() never has to search for
    // them (no allocation/locks on the audio thread).
    std::atomic<float>* loCutHz = nullptr;
    std::atomic<float>* hiCutHz = nullptr;
    std::atomic<float>* mixPercent = nullptr;
    std::atomic<float>* levelDb = nullptr;
    std::atomic<float>* irBlendPercent = nullptr;
    std::atomic<float>* micDistancePercent = nullptr;

    // v0.3.0 audio-thread-polled parameters.
    std::atomic<float>* blendModeChoice = nullptr;
    std::atomic<float>* irBTrimDb = nullptr;
    std::atomic<float>* irBPolarity = nullptr;
    std::atomic<float>* irBDelayMs = nullptr;
    std::atomic<float>* distanceAirOn = nullptr;
    std::atomic<float>* loCutSlopeChoice = nullptr;
    std::atomic<float>* hiCutSlopeChoice = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NaveAudioProcessor)
};
