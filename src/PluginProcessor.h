#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/CabConvolutionEngine.h"
#include "presets/PresetManager.h"

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

    juce::AudioProcessorValueTreeState apvts;

    // M2 preset system (.scaffold/specs/preset-system-m2.md,
    // src/presets/PresetManager.h). Constructed after apvts (its
    // constructor registers APVTS parameter listeners) and public so
    // NaveAudioProcessorEditor's PresetBar can talk to it directly - the
    // same "processor owns it, editor references it" pattern apvts itself
    // already uses.
    basilica::presets::PresetManager presetManager;

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
