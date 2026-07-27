#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "params/ParameterIds.h"
#include "params/ParameterLayout.h"
#include "state/IrStateSerialization.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <BinaryData.h>

#include <limits>

namespace
{
    // The small, Nave-specific config surface PresetManager needs (see
    // src/presets/PresetManager.h's class docs) - everything else about the
    // preset system is fully generic and portable to sibling plugins (see
    // docs/preset-system-notes.md).
    basilica::presets::PresetManagerConfig makePresetManagerConfig()
    {
        // JucePlugin_CFBundleIdentifier expands to a raw (unquoted) token
        // sequence, not a string literal - JUCE_STRINGIFY() is the
        // documented way to turn it into one (see JUCE's own
        // juce_CoreMidi_mac.mm for the same pattern). This is always
        // "com.yvesvogl.nave" here (BUNDLE_ID in CMakeLists.txt), matching
        // the "plugin" field baked into every presets/factory/*.json file.
        basilica::presets::PresetManagerConfig config;
        config.pluginId = JUCE_STRINGIFY (JucePlugin_CFBundleIdentifier);
        config.pluginName = JucePlugin_Name;
        config.manufacturerName = "Yves Vogl";
        config.pluginVersion = JucePlugin_VersionString;
        // userPresetsDirectoryOverrideForTests intentionally left
        // default-constructed (empty) - production instances always use the
        // real platform-standard preset location (see PresetManager.h).
        return config;
    }

    // BinaryData symbol names are derived from the presets/factory/*.json
    // file names passed to juce_add_binary_data() in CMakeLists.txt (dots
    // become underscores) - this list must stay in sync with that SOURCES
    // list. Order here only affects factory-preset iteration order before
    // getAllPresets() re-sorts alphabetically, so it isn't otherwise
    // significant.
    std::vector<basilica::presets::FactoryPresetAsset> makeFactoryPresetAssets()
    {
        return {
            { BinaryData::default_json, BinaryData::default_jsonSize },
            { BinaryData::tameTheFizz_json, BinaryData::tameTheFizz_jsonSize },
            { BinaryData::liveStage_json, BinaryData::liveStage_jsonSize },
            { BinaryData::darkVintage_json, BinaryData::darkVintage_jsonSize },
            { BinaryData::pushedBackInTheRoom_json, BinaryData::pushedBackInTheRoom_jsonSize },
            { BinaryData::touchOfRoomMic_json, BinaryData::touchOfRoomMic_jsonSize },
            { BinaryData::evenBlend_json, BinaryData::evenBlend_jsonSize },
            { BinaryData::parallelCabBlendedDry_json, BinaryData::parallelCabBlendedDry_jsonSize },
            { BinaryData::micMorph_json, BinaryData::micMorph_jsonSize },
            { BinaryData::tightStack_json, BinaryData::tightStack_jsonSize },
        };
    }
}

//==============================================================================
NaveAudioProcessor::NaveAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout()),
      presetManager (apvts, makePresetManagerConfig(), makeFactoryPresetAssets())
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

    blendModeChoice = apvts.getRawParameterValue (ParamIDs::blendMode);
    irBTrimDb = apvts.getRawParameterValue (ParamIDs::irBTrim);
    irBPolarity = apvts.getRawParameterValue (ParamIDs::irBPolarity);
    irBDelayMs = apvts.getRawParameterValue (ParamIDs::irBDelay);
    distanceAirOn = apvts.getRawParameterValue (ParamIDs::distanceAir);
    loCutSlopeChoice = apvts.getRawParameterValue (ParamIDs::loCutSlope);
    hiCutSlopeChoice = apvts.getRawParameterValue (ParamIDs::hiCutSlope);

    jassert (blendModeChoice != nullptr);
    jassert (irBTrimDb != nullptr);
    jassert (irBPolarity != nullptr);
    jassert (irBDelayMs != nullptr);
    jassert (distanceAirOn != nullptr);
    jassert (loCutSlopeChoice != nullptr);
    jassert (hiCutSlopeChoice != nullptr);

    // The four content-changing parameters get listeners rather than
    // per-block polling - see the AsyncUpdater rationale in PluginProcessor.h.
    for (const auto* id : { ParamIDs::alignMode, ParamIDs::irGainMode,
                             ParamIDs::irAMinPhase, ParamIDs::irBMinPhase })
        apvts.addParameterListener (id, this);

    // Presets carry parameter values but not the plugin's schema version, so
    // a preset written before v0.3.0 needs the same alignMode migration a
    // v0.2 session does - otherwise recalling an old preset would silently
    // upgrade its alignment maths and change how it sounds. Sharing
    // IrState::migrateParametersIfNeeded() with setStateInformation() means
    // the two paths can never drift apart.
    presetManager.setSchemaMigrationCallback ([this] (int declaredVersion)
    {
        IrState::migrateParametersIfNeeded (apvts, declaredVersion);
    });

    // M2 default resolution: user "Default" preset > factory "Default"
    // preset > the ParameterLayout defaults apvts was just constructed
    // with above (see PresetManager::applyStartupDefault()'s docs).
    presetManager.applyStartupDefault();
}

NaveAudioProcessor::~NaveAudioProcessor()
{
    // Both are essential and both must happen before any member is destroyed:
    // a listener left registered could fire into a half-destroyed processor,
    // and a pending async callback could do the same.
    for (const auto* id : { ParamIDs::alignMode, ParamIDs::irGainMode,
                             ParamIDs::irAMinPhase, ParamIDs::irBMinPhase })
        apvts.removeParameterListener (id, this);

    cancelPendingUpdate();
}

void NaveAudioProcessor::parameterChanged (const juce::String&, float)
{
    // Real-time safe and coalescing: several parameters changing in one block
    // produce a single reconfiguration on the message thread.
    triggerAsyncUpdate();
}

void NaveAudioProcessor::handleAsyncUpdate()
{
    reconfigureEngineFromParameters();
}

void NaveAudioProcessor::applyAudioThreadParameters()
{
    // The cheap half of the v0.3.0 parameter set: gains, delays, modes and
    // slopes, none of which allocate or touch impulse-response content, so
    // they can be pushed straight through from the audio thread every block
    // exactly as the v0.1/v0.2 parameters are.
    engine.setBlendMode (blendModeChoice->load (std::memory_order_relaxed) > 0.5f
                              ? CabConvolutionEngine::BlendMode::Morph
                              : CabConvolutionEngine::BlendMode::Crossfade);

    engine.setIrBTrimDb (irBTrimDb->load (std::memory_order_relaxed));
    engine.setIrBPolarityInverted (irBPolarity->load (std::memory_order_relaxed) > 0.5f);
    engine.setIrBDelayMs (irBDelayMs->load (std::memory_order_relaxed));
    engine.setDistanceAirEnabled (distanceAirOn->load (std::memory_order_relaxed) > 0.5f);

    engine.setLoCutSlope (loCutSlopeChoice->load (std::memory_order_relaxed) > 0.5f
                               ? CabConvolutionEngine::Slope::TwentyFourDbPerOctave
                               : CabConvolutionEngine::Slope::TwelveDbPerOctave);
    engine.setHiCutSlope (hiCutSlopeChoice->load (std::memory_order_relaxed) > 0.5f
                               ? CabConvolutionEngine::Slope::TwentyFourDbPerOctave
                               : CabConvolutionEngine::Slope::TwelveDbPerOctave);
}

void NaveAudioProcessor::reconfigureEngineFromParameters()
{
    const auto choiceIndex = [this] (const char* id)
    {
        if (auto* parameter = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (id)))
            return parameter->getIndex();

        return 0;
    };

    const auto boolValue = [this] (const char* id)
    {
        if (auto* parameter = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (id)))
            return parameter->get();

        return false;
    };

    // Order matters: gain mode and min-phase both reload slot content, and
    // alignMode re-derives slot B from whatever slot A ended up as. Setting
    // alignment last means IR B is aligned against the final IR A.
    engine.setGainMode (choiceIndex (ParamIDs::irGainMode) == 1
                             ? CabConvolutionEngine::GainMode::Loudness
                             : CabConvolutionEngine::GainMode::Energy);

    engine.setIrAMinPhase (boolValue (ParamIDs::irAMinPhase));
    engine.setIrBMinPhase (boolValue (ParamIDs::irBMinPhase));

    engine.setAlignMode (choiceIndex (ParamIDs::alignMode) == 0
                              ? IrAlignment::Mode::Legacy
                              : IrAlignment::Mode::Precise);
}

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
    // v0.3.0 correctness fix. Reporting 0 told the host the plugin had no tail,
    // so bounce/freeze/bypass operations truncated a decaying cabinet the
    // moment the source stopped. The real answer is the longer of the two
    // loaded IRs (0 while both slots hold the default delta, which genuinely
    // has no tail).
    return engine.getTailLengthSeconds();
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

    applyAudioThreadParameters();

    // Mono-in/stereo-out: the host has granted more output channels than input
    // channels, so the engine fills the extras from channel 0.
    engine.setDuplicateFirstChannel (getTotalNumInputChannels() == 1
                                      && getTotalNumOutputChannels() > 1);

    // The content-changing parameters are message-thread work, and
    // prepareToPlay() is a message-thread call, so they can be applied
    // directly here rather than going through the AsyncUpdater.
    reconfigureEngineFromParameters();

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

    // Matched layouts (mono->mono, stereo->stereo) as before, plus mono->stereo
    // (v0.3.0, survey gap #10): a guitarist's mono DI is the single most common
    // source for this plugin, and refusing the layout forced hosts to either
    // wrap it in a converter or play it out of one side. The engine duplicates
    // channel 0 into channel 1 at the top of the chain.
    if (mainIn == mainOut)
        return true;

    return mainIn == mono && mainOut == stereo;
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

    applyAudioThreadParameters();

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
void NaveAudioProcessor::embedImpulseResponsesIntoState()
{
    const auto embedSlot = [this] (int slotIndex, const char* property)
    {
        if (! engine.hasUserImpulseResponse (slotIndex))
        {
            // No user IR in this slot: clear any blob left from a previous
            // save, so loading the session does not resurrect an IR the user
            // has since cleared.
            apvts.state.removeProperty (property, nullptr);
            return;
        }

        // The RAW buffer is embedded - pre-alignment, pre-min-phase,
        // pre-normalisation. Saving the processed version would bake whatever
        // switches happened to be on at save time into the audio, so toggling
        // min-phase off after a reload would no longer restore the original.
        auto blob = IrState::encodeImpulseResponse (engine.getRawImpulseResponse (slotIndex),
                                                     engine.getRawImpulseResponseSampleRate (slotIndex));

        if (blob.getSize() > 0)
            apvts.state.setProperty (property, juce::var (std::move (blob)), nullptr);
        else
            apvts.state.removeProperty (property, nullptr);  // over the size cap: path-only
    };

    embedSlot (0, ParamIDs::irAudioAProperty);
    embedSlot (1, ParamIDs::irAudioBProperty);

    IrState::stampSchemaVersion (apvts.state);
}

void NaveAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    // Embedding mutates apvts.state, so it must happen before the snapshot.
    embedImpulseResponsesIntoState();

    const auto state = apvts.copyState();
    const std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void NaveAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState == nullptr || ! xmlState->hasTagName (apvts.state.getType()))
        return;

    const auto loadedState = juce::ValueTree::fromXml (*xmlState);
    const auto loadedSchemaVersion = IrState::readSchemaVersion (loadedState);

    apvts.replaceState (loadedState);

    // v1 -> v2 migration, before anything is loaded: it decides which
    // alignment maths IR B is about to be processed with.
    IrState::migrateParametersIfNeeded (apvts, loadedSchemaVersion);
    IrState::stampSchemaVersion (apvts.state);

    // The engine's message-thread configuration comes from parameters that
    // have just changed wholesale, and it governs how the IRs below are
    // processed - so it has to be applied first.
    reconfigureEngineFromParameters();

    restoreImpulseResponsesFromState();
}

void NaveAudioProcessor::restoreImpulseResponsesFromState()
{
    // setStateInformation() is a session/preset-load operation, never called
    // from the audio thread, so the blocking file I/O below is safe here. IR A
    // is restored first so it becomes the reference IR B's phase alignment is
    // computed against (see CabConvolutionEngine::setImpulseResponseB()),
    // matching how the two are loaded during normal interactive use (IR A
    // almost always loaded before IR B).
    //
    // Precedence, per slot: EMBEDDED AUDIO first, then the stored path, then
    // the default delta IR. Embedded audio is authoritative because it is the
    // only source that cannot have changed since the session was saved - the
    // file at the stored path may have been edited, replaced, or deleted.
    const auto restoreSlot = [this] (int slotIndex)
    {
        const auto* audioProperty = slotIndex == 0 ? ParamIDs::irAudioAProperty
                                                    : ParamIDs::irAudioBProperty;

        const auto loadEmbedded = [&]
        {
            const auto value = apvts.state.getProperty (audioProperty);

            if (const auto* blob = value.getBinaryData())
            {
                juce::AudioBuffer<float> buffer;
                double bufferSampleRate = 0.0;

                if (IrState::decodeImpulseResponse (*blob, buffer, bufferSampleRate))
                {
                    if (slotIndex == 0)
                        engine.setImpulseResponse (std::move (buffer), bufferSampleRate);
                    else
                        engine.setImpulseResponseB (std::move (buffer), bufferSampleRate);

                    return true;
                }
            }

            return false;
        };

        if (loadEmbedded())
            return;

        const auto path = slotIndex == 0 ? getCurrentIrFilePath() : getCurrentIrFilePathB();

        if (path.isNotEmpty())
        {
            const juce::File file (path);

            if (file.existsAsFile())
            {
                const auto loaded = slotIndex == 0 ? loadImpulseResponseFromFile (file)
                                                    : loadImpulseResponseFromFileB (file);

                if (loaded)
                    return;
            }
        }

        // No embedded audio and no readable file: fall back cleanly to the
        // delta IR rather than leaving whatever the previous session had.
        if (slotIndex == 0)
            loadDefaultImpulseResponse();
        else
            loadDefaultImpulseResponseB();
    };

    restoreSlot (0);
    restoreSlot (1);
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new NaveAudioProcessor();
}
