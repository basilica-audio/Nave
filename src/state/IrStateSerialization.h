#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

// Plugin state schema v2: embedded impulse-response audio, and the v1 -> v2
// migration.
//
// WHY THIS EXISTS. Through v0.2 a saved session stored only the IR files'
// absolute paths. That makes a project silently non-portable: move the IR
// folder, hand the session to a collaborator, or open it on another machine,
// and the plugin quietly falls back to a delta IR - the cabinet simply
// vanishes, with no error and no way to tell from the mix window that anything
// is missing. The survey called this the product's single worst defect, and it
// is: every commercial IR loader embeds its IR audio in the session.
//
// v0.3.0 stores the IR audio itself. Paths are kept alongside it as display
// and relocation metadata, so the UI can still show which file an IR came
// from, but the audio is authoritative on load.
//
// FORMAT. Each slot's blob is:
//
//   magic       4 bytes, 'NIR1'
//   sampleRate  float64, little-endian
//   numChannels int32,   little-endian
//   numSamples  int32,   little-endian
//   samples     float32 little-endian, CHANNEL-PLANAR (all of channel 0,
//               then all of channel 1, ...)
//
// gzip-compressed, then stored as a juce::MemoryBlock property on
// apvts.state, which JUCE base64-encodes when the tree is serialised to XML.
// Planar rather than interleaved because that is how juce::AudioBuffer already
// stores samples, so both directions are a straight memcpy per channel.
//
// SIZE. A typical cabinet IR is a few thousand taps - around 80 KB raw, and
// gzip does well on the near-silent tail. The per-slot cap below keeps a
// pathological load (someone loading a 30-second hall reverb into a cab
// slot) from producing a session chunk a host might refuse: past the cap the
// slot falls back to path-only, exactly as v0.2 behaved.
namespace IrState
{
    // The schema version stamped on apvts.state by this build. Absent means
    // v1, i.e. anything saved by v0.1/v0.2.
    inline constexpr int currentSchemaVersion = 2;
    inline constexpr int legacySchemaVersion = 1;

    // The longest IR that will be embedded, per slot. Longer IRs are saved
    // path-only (documented in docs/manual.md).
    inline constexpr double maximumEmbeddedSeconds = 10.0;

    inline constexpr int magicSize = 4;

    // Serialises `buffer` into the gzip'd blob described above. Returns an
    // empty MemoryBlock for an empty buffer, or one longer than
    // maximumEmbeddedSeconds at `sampleRate`.
    juce::MemoryBlock encodeImpulseResponse (const juce::AudioBuffer<float>& buffer, double sampleRate);

    // Reverses encodeImpulseResponse(). Returns false (leaving the outputs
    // untouched) if the blob is empty, truncated, not gzip, or does not carry
    // the expected magic - a corrupt or foreign blob must fall back to the
    // path, never load as noise.
    bool decodeImpulseResponse (const juce::MemoryBlock& data,
                                 juce::AudioBuffer<float>& destinationBuffer,
                                 double& destinationSampleRate);

    // The schema version recorded on `state`, defaulting to legacySchemaVersion
    // when the property is absent.
    int readSchemaVersion (const juce::ValueTree& state);

    // Stamps currentSchemaVersion onto `state`.
    void stampSchemaVersion (juce::ValueTree& state);

    // The v1 -> v2 parameter migration, shared by the processor's
    // setStateInformation() and the preset-apply path so the two can never
    // diverge.
    //
    // It does exactly one thing: force alignMode to Legacy. Every other new
    // v0.3.0 parameter defaults to a neutral value, so a v1 state that simply
    // lacks them already loads correctly. alignMode is the exception - its
    // default is Precise, because that is the right default for a NEW session,
    // but applying the better alignment algorithm to an existing project would
    // change how it sounds. A session saved in v0.2 must keep v0.2's alignment
    // maths; a user who wants the upgrade can switch the control themselves.
    //
    // Safe to call with a state that is already v2, in which case it does
    // nothing.
    void migrateParametersIfNeeded (juce::AudioProcessorValueTreeState& apvts, int loadedSchemaVersion);
}
