#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// The preset-format v1 LOADER, so a v2 preset can be parsed by it in-process.
//
// WHY A REPLICA. Decision D3 on Nave#42 requires that a preset written by a
// build that understands the IR reference still opens - with its parameters
// intact - in a build that does not. There is no way to link two versions of
// basilica::presets::PresetManager into one test binary (same class, same
// symbols), so the v1 parse path is reproduced here instead, in its own
// namespace, and the v2 document is run through it.
//
// PROVENANCE. Copied verbatim - control flow, comparisons and order of checks
// unchanged - from src/presets/PresetManager.cpp as it stood at commit
// 2afb1ac ("feat(ir): install the bundled IR library from the browser",
// the tip of main before this change), specifically:
//
//   PresetManager::parseAndValidate()   - the ONLY validation v1 performs
//   PresetManager::applyPlainValues()   - unknown parameter IDs ignored
//   PresetManager::applyParsedPreset()  - reset to defaults, then apply
//
// Reproduce with:
//   git show 2afb1ac:src/presets/PresetManager.cpp
//
// The one thing that had to change is the plumbing: v1 read its plugin id and
// format tag from PresetManagerConfig/PresetManager statics, and this takes
// them as arguments so the caller can pass v1's literal values.
//
// A REPLICA IS EVIDENCE ONLY IF IT CANNOT DRIFT. What actually keeps v1 and v2
// compatible is a single property of the real code: parseAndValidate() rejects
// on "format" and "plugin" and on nothing else, so an unknown top-level key
// survives it. tests/PresetIrReferenceTests.cpp therefore pins that property
// directly as well - that presetFormatTag still reads "basilica-preset-1", and
// that this replica rejects a document whose tag is anything else. If a future
// change bumps the tag, that test fails, which is the real regression guard;
// this file is what demonstrates the consequence.
namespace v1replica
{
    // v1's presetFormatTag (src/presets/PresetManager.h @ 2afb1ac). Spelled
    // out rather than referenced so this file keeps testing what v1 DID,
    // even if the constant is ever changed.
    inline constexpr const char* formatTag = "basilica-preset-1";

    struct LoadResult
    {
        bool succeeded = false;
        juce::String errorMessage;
    };

    // v1 PresetManager::parseAndValidate(), verbatim.
    inline juce::var parseAndValidate (const juce::String& jsonText,
                                        const juce::String& pluginId,
                                        juce::String& errorMessage)
    {
        juce::var parsed;
        const auto parseResult = juce::JSON::parse (jsonText, parsed);

        if (parseResult.failed() || ! parsed.isObject())
        {
            errorMessage = TRANS ("This file is not a valid preset.");
            return {};
        }

        auto* obj = parsed.getDynamicObject();

        if (obj->getProperty ("format").toString() != juce::String (formatTag))
        {
            errorMessage = TRANS ("This preset was saved by an incompatible version of the preset format.");
            return {};
        }

        if (obj->getProperty ("plugin").toString() != pluginId)
        {
            errorMessage = TRANS ("This preset file belongs to a different plugin.");
            return {};
        }

        return parsed;
    }

    // v1 PresetManager::applyPlainValues(), verbatim.
    inline void applyPlainValues (juce::AudioProcessorValueTreeState& apvts,
                                   const juce::var& parametersObject)
    {
        auto* obj = parametersObject.getDynamicObject();

        if (obj == nullptr)
            return;

        for (auto& property : obj->getProperties())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (apvts.getParameter (property.name.toString()));

            if (ranged != nullptr)
                ranged->setValueNotifyingHost (ranged->convertTo0to1 (static_cast<float> (property.value)));
        }
    }

    // v1 PresetManager::loadPreset()'s body for a user preset file, i.e.
    // parseAndValidate() followed by applyParsedPreset()'s
    // reset-to-defaults-then-apply. v1 had no notion of extra top-level
    // fields, so there is nothing here that could look at one.
    inline LoadResult loadPresetJson (juce::AudioProcessorValueTreeState& apvts,
                                       const juce::String& jsonText,
                                       const juce::String& pluginId)
    {
        LoadResult result;
        const auto parsed = parseAndValidate (jsonText, pluginId, result.errorMessage);

        if (parsed.isVoid())
            return result;

        for (auto* parameter : apvts.processor.getParameters())
            if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (parameter))
                ranged->setValueNotifyingHost (ranged->getDefaultValue());

        applyPlainValues (apvts, parsed.getDynamicObject()->getProperty ("parameters"));

        result.succeeded = true;
        return result;
    }
}
