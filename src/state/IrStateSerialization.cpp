#include "IrStateSerialization.h"

#include "../params/ParameterIds.h"

#include <juce_dsp/juce_dsp.h>

namespace
{
    // 'NIR1' - Nave Impulse Response, format 1. Checked on decode so a blob
    // written by some other plugin (or a truncated one) is rejected rather
    // than reinterpreted as samples.
    constexpr char magic[IrState::magicSize] = { 'N', 'I', 'R', '1' };

    constexpr int headerSize = IrState::magicSize
                                + static_cast<int> (sizeof (double))   // sampleRate
                                + static_cast<int> (sizeof (juce::int32))  // numChannels
                                + static_cast<int> (sizeof (juce::int32)); // numSamples

    // Sanity bounds, so a corrupt header can never make the decoder try to
    // allocate an absurd buffer before it notices the data is short.
    constexpr int maximumChannels = 8;
    constexpr int maximumSamples = 32 * 1024 * 1024;
}

namespace IrState
{
    juce::MemoryBlock encodeImpulseResponse (const juce::AudioBuffer<float>& buffer, double sampleRate)
    {
        const auto numChannels = buffer.getNumChannels();
        const auto numSamples = buffer.getNumSamples();

        if (numChannels <= 0 || numSamples <= 0 || sampleRate <= 0.0)
            return {};

        // Past the cap the slot stays path-only, exactly as v0.2 behaved, so a
        // pathologically long IR cannot bloat the host's session file.
        if (static_cast<double> (numSamples) / sampleRate > maximumEmbeddedSeconds)
            return {};

        if (numChannels > maximumChannels || numSamples > maximumSamples)
            return {};

        juce::MemoryBlock compressed;

        {
            juce::MemoryOutputStream compressedStream (compressed, false);

            // Scoped so the compressor flushes its gzip trailer before
            // `compressed` is read below - without the scope the blob would be
            // truncated and would fail its own decode.
            juce::GZIPCompressorOutputStream gzip (compressedStream);

            gzip.write (magic, magicSize);
            gzip.writeDouble (sampleRate);
            gzip.writeInt (numChannels);
            gzip.writeInt (numSamples);

            for (int channel = 0; channel < numChannels; ++channel)
            {
                const auto* source = buffer.getReadPointer (channel);

                for (int i = 0; i < numSamples; ++i)
                    gzip.writeFloat (source[i]);
            }
        }

        return compressed;
    }

    bool decodeImpulseResponse (const juce::MemoryBlock& data,
                                 juce::AudioBuffer<float>& destinationBuffer,
                                 double& destinationSampleRate)
    {
        if (data.getSize() == 0)
            return false;

        // Decompressed whole before anything is parsed, so the payload's length
        // can be validated against the header BEFORE any samples are read. The
        // alternative - reading straight from the gzip stream - would happily
        // return zeros past the end of a truncated blob and present half an IR
        // as though it were complete.
        juce::MemoryBlock plain;

        {
            juce::MemoryInputStream compressedStream (data, false);
            juce::GZIPDecompressorInputStream gzip (compressedStream);

            gzip.readIntoMemoryBlock (plain);
        }

        if (plain.getSize() < static_cast<size_t> (headerSize))
            return false;

        juce::MemoryInputStream stream (plain, false);

        char readMagic[magicSize] = {};

        if (stream.read (readMagic, magicSize) != magicSize
            || std::memcmp (readMagic, magic, magicSize) != 0)
            return false;

        const auto sampleRate = stream.readDouble();
        const auto numChannels = stream.readInt();
        const auto numSamples = stream.readInt();

        if (! (sampleRate > 0.0)
            || numChannels <= 0 || numChannels > maximumChannels
            || numSamples <= 0 || numSamples > maximumSamples)
            return false;

        const auto expectedBytes = static_cast<size_t> (headerSize)
                                    + static_cast<size_t> (numChannels)
                                          * static_cast<size_t> (numSamples)
                                          * sizeof (float);

        if (plain.getSize() < expectedBytes)
            return false;

        juce::AudioBuffer<float> buffer (numChannels, numSamples);

        for (int channel = 0; channel < numChannels; ++channel)
        {
            auto* destination = buffer.getWritePointer (channel);

            for (int i = 0; i < numSamples; ++i)
                destination[i] = stream.readFloat();
        }

        destinationBuffer = std::move (buffer);
        destinationSampleRate = sampleRate;

        return true;
    }

    int readSchemaVersion (const juce::ValueTree& state)
    {
        return static_cast<int> (state.getProperty (ParamIDs::stateVersionProperty, legacySchemaVersion));
    }

    void stampSchemaVersion (juce::ValueTree& state)
    {
        state.setProperty (ParamIDs::stateVersionProperty, currentSchemaVersion, nullptr);
    }

    void migrateParametersIfNeeded (juce::AudioProcessorValueTreeState& apvts, int loadedSchemaVersion)
    {
        if (loadedSchemaVersion >= currentSchemaVersion)
            return;

        // See the header for why alignMode is the only parameter that needs
        // touching: every other v0.3.0 addition defaults to a neutral value,
        // so a v1 state that simply lacks them already loads correctly.
        if (auto* parameter = apvts.getParameter (ParamIDs::alignMode))
        {
            // Index 0 == Legacy in the choice list (src/params/ParameterLayout.cpp).
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (0.0f));
        }
    }
}
