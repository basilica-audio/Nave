#pragma once

#include <juce_core/juce_core.h>

#include <vector>

// Wave-3 compositional-GUI layout manifest (resources/gui/layout_manifest.json,
// embedded via BinaryData and parsed once, on the message thread, at editor
// construction). The manifest is the SINGLE source of truth for what sits
// where on the plate: per control {APVTS parameter id (or slot/meter id),
// element kind, sprite family, centre position in the plate render's own
// pixel space (1264x848 for the 1k family plates), draw scale, engraved
// label}. PluginEditor.cpp composites from it, and tests/gui/
// EditorLayoutTests.cpp asserts its invariants (control count vs. the
// rollout control inventory, row alignment, keep-out zones) against the
// exact same parsed data - never a second hand-maintained coordinate list.
//
// Deliberately design-agnostic (the suite component-family convention, see
// PlateTypography.h's identical note): no BinaryData.h include here - the
// raw JSON bytes are passed in by the editor/tests, so this file is
// copyable verbatim to sibling plugins whose BinaryData namespaces differ.
namespace basilica::gui
{
    struct ManifestControl
    {
        juce::String id;     // APVTS parameter id; for kind "meter"/"slot" a plugin-defined element id
        juce::String kind;   // "knob" | "selector" | "toggle" | "meter" | "slot"
        juce::String sprite; // sprite-family key, resolved to an embedded image by the editor
        juce::String label;  // engraved plate lettering (empty = no label)
        float cx = 0.0f, cy = 0.0f; // element anchor centre, plate-render px
        float scale = 1.0f;         // sprite draw scale, plate-render px per sprite px
        float labelCy = 0.0f;       // engraved label centre-line y, plate-render px (0 = no label row)
    };

    struct LayoutManifest
    {
        int plateWidthPx = 0, plateHeightPx = 0;
        std::vector<ManifestControl> controls;

        bool isValid() const noexcept
        {
            return plateWidthPx > 0 && plateHeightPx > 0 && ! controls.empty();
        }

        std::vector<const ManifestControl*> ofKind (const juce::String& kind) const
        {
            std::vector<const ManifestControl*> result;

            for (const auto& control : controls)
                if (control.kind == kind)
                    result.push_back (&control);

            return result;
        }

        const ManifestControl* findById (const juce::String& id) const noexcept
        {
            for (const auto& control : controls)
                if (control.id == id)
                    return &control;

            return nullptr;
        }

        // Parses the embedded manifest JSON. Returns an empty/invalid
        // manifest (isValid() == false) on any structural error - the
        // editor jasserts on that and renders plate-only rather than
        // crashing; tests fail loudly on the same condition.
        static LayoutManifest parse (const char* jsonData, int jsonSize)
        {
            LayoutManifest manifest;

            const auto parsed = juce::JSON::parse (juce::String::fromUTF8 (jsonData, jsonSize));

            const auto* root = parsed.getDynamicObject();
            if (root == nullptr)
                return manifest;

            const auto plate = root->getProperty ("plate");
            if (const auto* plateObject = plate.getDynamicObject())
            {
                manifest.plateWidthPx = (int) plateObject->getProperty ("widthPx");
                manifest.plateHeightPx = (int) plateObject->getProperty ("heightPx");
            }

            const auto controls = root->getProperty ("controls");
            if (const auto* controlArray = controls.getArray())
            {
                for (const auto& entry : *controlArray)
                {
                    const auto* object = entry.getDynamicObject();
                    if (object == nullptr)
                        return {};

                    ManifestControl control;
                    control.id = object->getProperty ("id").toString();
                    control.kind = object->getProperty ("kind").toString();
                    control.sprite = object->getProperty ("sprite").toString();
                    control.label = object->getProperty ("label").toString();
                    control.cx = (float) (double) object->getProperty ("cx");
                    control.cy = (float) (double) object->getProperty ("cy");
                    control.scale = object->hasProperty ("scale") ? (float) (double) object->getProperty ("scale") : 1.0f;
                    control.labelCy = object->hasProperty ("labelCy") ? (float) (double) object->getProperty ("labelCy") : 0.0f;

                    if (control.id.isEmpty() || control.kind.isEmpty() || control.sprite.isEmpty())
                        return {};

                    manifest.controls.push_back (std::move (control));
                }
            }

            return manifest;
        }
    };
}
