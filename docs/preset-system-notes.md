# M2 preset system - implementation notes and sibling replication recipe

Nave is the **pilot** implementation of the Basilica Audio suite-wide M2
preset system (binding spec: `.scaffold/specs/preset-system-m2.md`). This
document is the replication recipe for the other 11 plugins: what to copy
verbatim, what small per-plugin glue to write, and the CMake/BinaryData
wiring it needs.

## Files and their portability

| File | Portability |
|---|---|
| `src/presets/PresetManager.h` / `.cpp` | **Copy verbatim.** Zero Nave-specific code - all plugin-specific data flows in through the constructor's `PresetManagerConfig` + `std::vector<FactoryPresetAsset>` arguments. |
| `src/presets/PresetBar.h` / `.cpp` | **Copy verbatim.** Only depends on `PresetManager`'s public API. |
| `src/presets/Localisation.h` / `.cpp` | **Copy verbatim.** Takes the German `BinaryData::` symbol pointer/size as arguments rather than including `BinaryData.h` itself. |
| `resources/i18n/de.txt` | **Copy verbatim** if the frame strings are identical (they should be - PresetBar's button/menu/dialog text never changes across plugins). If a sibling's PresetBar ever needs additional TRANS()'d strings beyond what Nave's PresetBar uses, add the extra key/value pairs here too. |
| `presets/factory/*.json` | **Do not copy.** Each plugin's factory preset *content* is designed per plugin by its own implementation agent (spec: "factory preset CONTENT is designed per plugin by its implementation agent") - only the JSON *format* (`"basilica-preset-1"`) is shared. |
| `PluginProcessor.cpp`'s `makePresetManagerConfig()`/`makeFactoryPresetAssets()` helpers | **Rewrite per plugin.** This is the "small config surface" - see below. |
| `PluginProcessor.h`'s `presetManager` member + `PluginProcessor.cpp`'s constructor wiring | **Adapt per plugin** (mechanical - same pattern, different processor class name). |
| `PluginEditor.h`/`.cpp`'s `presetBar` member + layout | **Adapt per plugin** (mechanical - same pattern, different editor class/layout). |
| `docs/presets.md` | **Do not copy.** Per-plugin factory preset documentation. |

## The per-plugin config surface

Everything PresetManager needs beyond generic file-format/dirty-tracking/
ordering logic is exactly these four fields (`PresetManagerConfig` in
`PresetManager.h`):

```cpp
struct PresetManagerConfig
{
    juce::String pluginId;             // e.g. "com.yvesvogl.overture" - must match BUNDLE_ID
    juce::String pluginName;           // e.g. "Overture" - JucePlugin_Name
    juce::String manufacturerName;     // "Yves Vogl" for every suite plugin
    juce::String pluginVersion;        // JucePlugin_VersionString
    juce::File userPresetsDirectoryOverrideForTests; // leave default-constructed in production
};
```

`pluginId` comes from `JUCE_STRINGIFY (JucePlugin_CFBundleIdentifier)` (a
JUCE-generated macro that expands to a raw, unquoted token sequence -
`JUCE_STRINGIFY()` is required to turn it into a string literal; see
`PluginProcessor.cpp`'s `makePresetManagerConfig()` for the exact pattern and
its comment on why the naive `JucePlugin_CFBundleIdentifier` alone would not
compile).

`FactoryPresetAsset` (also in `PresetManager.h`) is just a `{ const char*
data; int dataSize; }` pair - built from each plugin's own
`BinaryData::<name>_json` / `BinaryData::<name>_jsonSize` symbols in that
plugin's own `makeFactoryPresetAssets()` helper.

## CMake / BinaryData wiring checklist

1. Add `ICON_BIG "docs/assets/icon.png"` to the plugin's own `juce_add_plugin(...)` call (unrelated to presets, but bundled in the same v0.2.0 wave - see the top-level `juce_add_plugin` call in `CMakeLists.txt`).
2. Author `presets/factory/*.json` for the plugin (6-10 presets per spec, one `Init`-category `Default`, English names, no brand names).
3. Author (or copy) `resources/i18n/de.txt`.
4. Add a `juce_add_binary_data(<Plugin>BinaryData SOURCES ...)` call listing every `presets/factory/*.json` file plus `resources/i18n/de.txt` (see `CMakeLists.txt`'s block right after `juce_add_plugin`). BinaryData symbol names are derived from file names with `.`/other invalid characters replaced by `_` (e.g. `default.json` -> `BinaryData::default_json`/`BinaryData::default_jsonSize`) - **avoid leading digits in preset file names**, since a leading digit would produce an invalid C++ identifier.
5. Link `<Plugin>BinaryData` and `juce::juce_gui_basics` into `SharedCode` (PresetBar needs `juce_gui_basics` for `AlertWindow`/`FileChooser`/`PopupMenu`; most plugin editors already pull it in via `juce_audio_utils`, but linking it explicitly is what Nave now does and costs nothing extra).
6. `#include <BinaryData.h>` in `PluginProcessor.cpp` (factory assets) and `PluginEditor.cpp` (the `de.txt` symbols for `installLocalisation()`).

## Processor/editor wiring checklist

**`PluginProcessor.h`**: add `#include "presets/PresetManager.h"` and a
public `basilica::presets::PresetManager presetManager;` member, declared
**after** `apvts` (construction order follows declaration order - see the
comment on this member in Nave's `PluginProcessor.h`).

**`PluginProcessor.cpp`**: add `makePresetManagerConfig()`/
`makeFactoryPresetAssets()` helpers (anonymous namespace), construct
`presetManager` in the constructor's initialiser list right after `apvts`,
and call `presetManager.applyStartupDefault();` in the constructor body
(after the parameter-pointer `jassert`s, before the closing brace) so a
fresh instance loads the user-or-factory `Default` preset per the spec's
default-resolution order.

**`PluginEditor.h`**: add `#include "presets/PresetBar.h"` and a
`basilica::presets::PresetBar presetBar;` member.

**`PluginEditor.cpp`**: the trickiest part is **initialisation order**.
`installLocalisation()` must run *before* `presetBar`'s own constructor
calls `TRANS()` on its button labels - and since C++ initialises members in
declaration order regardless of the order they're written in the
initialiser list, a plain `installLocalisation()` call in the constructor
*body* runs too late (after `presetBar` already exists). Nave's
`PluginEditor.cpp` solves this with a small helper function called from
`presetBar`'s own initialiser expression:

```cpp
namespace
{
    basilica::presets::PresetManager& initLocalisationThenGetPresetManager (NaveAudioProcessor& processor)
    {
        basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize);
        return processor.presetManager;
    }
}

NaveAudioProcessorEditor::NaveAudioProcessorEditor (NaveAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      audioProcessor (processorToEdit),
      presetBar (initLocalisationThenGetPresetManager (processorToEdit))
{
    addAndMakeVisible (presetBar);
    // ... rest of the editor's existing setup, shifted down to make room
    // for presetBar at the top of resized()'s layout ...
}
```

Copy this pattern (renaming the processor/editor class names) rather than
re-deriving it - the ordering bug it avoids is easy to reintroduce
otherwise.

Finally, in `resized()`, reserve a row at the very top of the editor for
`presetBar.setBounds(...)` before laying out the plugin's existing controls.

## Real-time safety (for the doc-discipline record)

`PresetManager`'s only audio-thread-adjacent code is its private
`AudioProcessorValueTreeState::Listener::parameterChanged()` override, used
for dirty-flag tracking. JUCE 8.0.14 does not document that callback as
guaranteed message-thread-only (host automation could in principle deliver
it from another thread), so it is implemented as a single lock-free
`std::atomic<bool>` store and nothing else. Every other `PresetManager`/
`PresetBar` method does file I/O, JSON parsing, and `juce::String`/`juce::var`
allocation, and is called only from the message thread (constructor,
`PresetBar` button/menu/dialog callbacks) - never from `processBlock()`.

## Test isolation

`PresetManagerConfig::userPresetsDirectoryOverrideForTests` exists purely so
`tests/PresetManagerTests.cpp` never reads or writes the real per-user
preset directory on the machine running the tests. Production code (the
`PluginProcessor.cpp` helpers above) always leaves this field
default-constructed (empty), so real plugin instances use the genuine
platform-standard location
(`~/Library/Audio/Presets/Yves Vogl/<Plugin>/` on macOS,
`%APPDATA%/Yves Vogl/<Plugin>/Presets/` on Windows). Copy this pattern into
every sibling's own preset tests.

## What Nave's v0.2.0 pass did *not* build

- No bespoke preset-browser UI beyond the plain `PresetBar` strip - matches the spec's explicit "M3 restyles it, do not gold-plate" instruction.
- `PresetBar` only exposes single-preset export via its "Export..." button; `PresetManager::exportBank()`/`importBank()` (zip banks) are fully implemented and tested at the `PresetManager` layer (`tests/PresetManagerTests.cpp`), but bank export isn't yet wired to a dedicated UI affordance - `promptAndImport()` does auto-detect and import `.zip` banks, so bank *import* is reachable through the existing "Import..." button today. A future M3 pass can add an explicit "Export Bank..." menu item without any `PresetManager`-level changes.
