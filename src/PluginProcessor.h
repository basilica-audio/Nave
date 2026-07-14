#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/CabConvolutionEngine.h"

// Nave: a cabinet impulse-response (IR) loader for reamping guitar/bass DI
// tracks. Signal flow lives in CabConvolutionEngine (src/dsp) so it stays
// unit-testable independent of this AudioProcessor; this class is just
// APVTS + host plumbing + IR file I/O around it.
class NaveAudioProcessor final : public juce::AudioProcessor
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

private:
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

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (NaveAudioProcessor)
};
