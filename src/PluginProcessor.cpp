#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "params/ParameterIds.h"
#include "params/ParameterLayout.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <limits>

//==============================================================================
NaveAudioProcessor::NaveAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    loCutHz = apvts.getRawParameterValue (ParamIDs::loCut);
    hiCutHz = apvts.getRawParameterValue (ParamIDs::hiCut);
    mixPercent = apvts.getRawParameterValue (ParamIDs::mix);
    levelDb = apvts.getRawParameterValue (ParamIDs::level);
    irBlendPercent = apvts.getRawParameterValue (ParamIDs::irBlend);
    micDistancePercent = apvts.getRawParameterValue (ParamIDs::micDistance);

    jassert (loCutHz != nullptr);
    jassert (hiCutHz != nullptr);
    jassert (mixPercent != nullptr);
    jassert (levelDb != nullptr);
    jassert (irBlendPercent != nullptr);
    jassert (micDistancePercent != nullptr);
}

NaveAudioProcessor::~NaveAudioProcessor() = default;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout NaveAudioProcessor::createParameterLayout()
{
    return nave::createParameterLayout();
}

//==============================================================================
const juce::String NaveAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool NaveAudioProcessor::acceptsMidi() const
{
    return false;
}

bool NaveAudioProcessor::producesMidi() const
{
    return false;
}

bool NaveAudioProcessor::isMidiEffect() const
{
    return false;
}

double NaveAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int NaveAudioProcessor::getNumPrograms()
{
    return 1;
}

int NaveAudioProcessor::getCurrentProgram()
{
    return 0;
}

void NaveAudioProcessor::setCurrentProgram (int)
{
}

const juce::String NaveAudioProcessor::getProgramName (int)
{
    return {};
}

void NaveAudioProcessor::changeProgramName (int, const juce::String&)
{
}

//==============================================================================
void NaveAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (getTotalNumOutputChannels());

    // Seed the engine's parameters from the current APVTS state before
    // prepare() primes filter coefficients/convolution state, so the very
    // first block after prepareToPlay() already reflects the host/session's
    // actual parameter values rather than the engine's built-in defaults.
    engine.setLoCutHz (loCutHz->load (std::memory_order_relaxed));
    engine.setHiCutHz (hiCutHz->load (std::memory_order_relaxed));
    engine.setMixProportion (mixPercent->load (std::memory_order_relaxed) * 0.01f);
    engine.setLevelDb (levelDb->load (std::memory_order_relaxed));
    engine.setBlendProportion (irBlendPercent->load (std::memory_order_relaxed) * 0.01f);
    engine.setDistancePercent (micDistancePercent->load (std::memory_order_relaxed));

    engine.prepare (spec);

    // The convolution engine is the only potential source of reported
    // latency (zero for the default zero-latency configuration this engine
    // always uses, but reported generically); the dry path is delay-
    // compensated against it internally by CabConvolutionEngine's
    // DryWetMixer (see docs/architecture.md).
    setLatencySamples (engine.getLatencySamples());
}

void NaveAudioProcessor::releaseResources()
{
}

void NaveAudioProcessor::reset()
{
    engine.reset();
}

bool NaveAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();

    const auto mainOut = layouts.getMainOutputChannelSet();
    const auto mainIn = layouts.getMainInputChannelSet();

    if (mainOut != mono && mainOut != stereo)
        return false;

    if (mainOut != mainIn)
        return false;

    return true;
}

void NaveAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Buses are constrained to in == out (mono or stereo), so this is
    // normally a no-op, but it's cheap insurance against stray channels.
    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    engine.setLoCutHz (loCutHz->load (std::memory_order_relaxed));
    engine.setHiCutHz (hiCutHz->load (std::memory_order_relaxed));
    engine.setMixProportion (mixPercent->load (std::memory_order_relaxed) * 0.01f);
    engine.setLevelDb (levelDb->load (std::memory_order_relaxed));
    engine.setBlendProportion (irBlendPercent->load (std::memory_order_relaxed) * 0.01f);
    engine.setDistancePercent (micDistancePercent->load (std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);
}

//==============================================================================
bool NaveAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* NaveAudioProcessor::createEditor()
{
    return new NaveAudioProcessorEditor (*this);
}

//==============================================================================
bool NaveAudioProcessor::loadImpulseResponseFromFile (const juce::File& irFile)
{
    if (! irFile.existsAsFile())
        return false;

    // AudioFormatManager is local and short-lived: registering formats is
    // cheap, and the whole file read below completes before it goes out of
    // scope, so the reader never outlives the manager it came from. This
    // method is documented as message-thread/off-audio-thread only, so the
    // blocking file I/O here is safe.
    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    const std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (irFile));

    if (reader == nullptr)
        return false;

    const auto numChannels = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));
    const auto numSamples = static_cast<int> (juce::jmin<juce::int64> (reader->lengthInSamples,
                                                                        static_cast<juce::int64> (std::numeric_limits<int>::max())));

    if (numSamples <= 0)
        return false;

    juce::AudioBuffer<float> irBuffer (numChannels, numSamples);
    reader->read (&irBuffer, 0, numSamples, 0, true, true);

    engine.setImpulseResponse (std::move (irBuffer), reader->sampleRate);

    // Persisted directly on the live APVTS state (rather than as an APVTS
    // float parameter - a file path has no meaningful numeric
    // representation) so it round-trips through the normal
    // getStateInformation()/setStateInformation() flow alongside the
    // regular parameters.
    apvts.state.setProperty (ParamIDs::irFilePathProperty, irFile.getFullPathName(), nullptr);

    return true;
}

void NaveAudioProcessor::loadDefaultImpulseResponse()
{
    engine.loadDefaultImpulseResponse();
    apvts.state.setProperty (ParamIDs::irFilePathProperty, juce::String(), nullptr);
}

juce::String NaveAudioProcessor::getCurrentIrFilePath() const
{
    return apvts.state.getProperty (ParamIDs::irFilePathProperty, juce::String()).toString();
}

bool NaveAudioProcessor::loadImpulseResponseFromFileB (const juce::File& irFile)
{
    if (! irFile.existsAsFile())
        return false;

    juce::AudioFormatManager formatManager;
    formatManager.registerBasicFormats();

    const std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (irFile));

    if (reader == nullptr)
        return false;

    const auto numChannels = juce::jlimit (1, 2, static_cast<int> (reader->numChannels));
    const auto numSamples = static_cast<int> (juce::jmin<juce::int64> (reader->lengthInSamples,
                                                                        static_cast<juce::int64> (std::numeric_limits<int>::max())));

    if (numSamples <= 0)
        return false;

    juce::AudioBuffer<float> irBuffer (numChannels, numSamples);
    reader->read (&irBuffer, 0, numSamples, 0, true, true);

    engine.setImpulseResponseB (std::move (irBuffer), reader->sampleRate);

    apvts.state.setProperty (ParamIDs::irFilePathBProperty, irFile.getFullPathName(), nullptr);

    return true;
}

void NaveAudioProcessor::loadDefaultImpulseResponseB()
{
    engine.loadDefaultImpulseResponseB();
    apvts.state.setProperty (ParamIDs::irFilePathBProperty, juce::String(), nullptr);
}

juce::String NaveAudioProcessor::getCurrentIrFilePathB() const
{
    return apvts.state.getProperty (ParamIDs::irFilePathBProperty, juce::String()).toString();
}

//==============================================================================
void NaveAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    const std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void NaveAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState == nullptr || ! xmlState->hasTagName (apvts.state.getType()))
        return;

    apvts.replaceState (juce::ValueTree::fromXml (*xmlState));

    // setStateInformation() is a session/preset-load operation, never called
    // from the audio thread, so the blocking file I/O in
    // loadImpulseResponseFromFile()/loadImpulseResponseFromFileB() is safe
    // here. IR A is restored first so it becomes the reference IR B's phase
    // alignment is computed against (see CabConvolutionEngine::
    // setImpulseResponseB()), matching how the two are loaded during normal
    // interactive use (IR A almost always loaded before IR B).
    const auto irPath = getCurrentIrFilePath();

    if (irPath.isNotEmpty())
    {
        const juce::File irFile (irPath);

        if (irFile.existsAsFile())
            loadImpulseResponseFromFile (irFile);
        else
            loadDefaultImpulseResponse(); // stored IR is missing; fall back cleanly
    }
    else
    {
        loadDefaultImpulseResponse();
    }

    const auto irPathB = getCurrentIrFilePathB();

    if (irPathB.isNotEmpty())
    {
        const juce::File irFileB (irPathB);

        if (irFileB.existsAsFile())
            loadImpulseResponseFromFileB (irFileB);
        else
            loadDefaultImpulseResponseB(); // stored IR is missing; fall back cleanly
    }
    else
    {
        loadDefaultImpulseResponseB();
    }
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NaveAudioProcessor();
}
