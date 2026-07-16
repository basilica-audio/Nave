# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.2.0] - 2026-07-16

Deep-dive voicing pass (research-derived, sourced from manuals/forums/physics standards - not measured hardware, see `docs/design-brief.md`'s Honesty section) plus the suite's first M2 preset system implementation (pilot for the other 11 plugins), a German frame-string localisation, and an app icon. Also folds in a run of fixes/housekeeping commits merged after v0.1.0 that never got their own release.

### Changed

- **Distance's low-shelf taper is now front-loaded, not linear** (`docs/design-brief.md`'s "Distance" module spec): real proximity effect concentrates most of its audible change early in a mic's travel and saturates, rather than growing evenly - `CabConvolutionEngine`'s low-shelf cut now scales through an "ease-out" power curve (`1 - (1 - normalisedDistance)^1.8`) instead of plain-linear-in-dB. The high-shelf keeps its linear taper (a deliberate asymmetry - see `docs/architecture.md`'s "v0.2.0 Distance taper" note). **This is parameter-behavior-breaking pre-1.0**: a session with Distance at, say, 60% will sound different after this update, even though the parameter's own range/default are unchanged and no migration is needed (see the design brief's Versioning section).
- **Doc/manual wording**: replaced "air-absorption darkening" framing for Distance's high-shelf with a directivity-first explanation (`docs/manual.md`, `docs/architecture.md`, `README.md`) - the audible effect is real, but at typical reamping distances it's driven far more by loudspeaker directivity than literal atmospheric absorption.
- **LoCut/HiCut ranges (20-800 Hz / 2-20 kHz) are now explicitly documented as a deliberate keep**, not an unexamined default, after comparison against the closest structural reference plugin (Ignite Amps NadIR: 10-400 Hz / 6-22 kHz) - see `docs/architecture.md` and the new named regression test in `tests/EngineTests.cpp`. No range or default values changed.

### Added

- **M2 preset system** (`.scaffold/specs/preset-system-m2.md`, binding suite-wide spec - Nave is the pilot implementation): `src/presets/PresetManager` (factory presets embedded via BinaryData, user presets on disk, load/save/rename/delete, dirty-state tracking, prev/next navigation, single-file and zip-bank import/export, user-preset-wins-over-factory default resolution) and `src/presets/PresetBar` (the editor strip, docked at the top of the existing v0.1/v0.2 layout). Written with no Nave-specific coupling beyond a small config struct, documented as a sibling-plugin replication recipe in `docs/preset-system-notes.md`.
- **8 factory presets** (`presets/factory/*.json`, documented in `docs/presets.md`): Default, Tame the Fizz, Live Stage, Dark Vintage, Pushed Back in the Room, Touch of Room Mic, Even Blend, Parallel Cab (Blended Dry) - sourced starting points from `docs/design-brief.md`, tunable/auditionable, not claimed as hardware-matched.
- **German frame-string localisation**: `resources/i18n/de.txt`, selected automatically via `SystemStats::getUserLanguage()` for PresetBar's labels/menus/dialogs. Parameter/DSP terminology (LoCut, HiCut, Distance, Mix, Level, Hz, dB, %) is never translated, in this pass or any future one.
- **App icon wired into the plugin binaries** (`ICON_BIG` in `CMakeLists.txt`, pointing at the icon asset added in v0.1.0's branding pass) - previously only used in docs/README, not embedded in the built AU/VST3/Standalone.
- `docs/design-brief.md` and `docs/research-notes.md`: the sourced design brief and research notes this release's voicing/doc changes are derived from.
- Manual now notes that loading two different real-world IRs can land at different output levels because `Convolution::Normalise::yes` is an energy normalisation, not a perceptual loudness match - pointing at Level as the fix (`docs/manual.md`).
- New Catch2 coverage: Distance taper front-loading/monotonicity test, a fixed-point taper regression snapshot, a named LoCut/HiCut range-guard test, and 16 preset-system tests (`tests/PresetManagerTests.cpp` - round-trip, forward-tolerant import, wrong-plugin/wrong-format refusal, factory preset integrity, default resolution order, dirty-flag lifecycle, prev/next wrap-around, rename/delete guards, single-file and bank import/export).

### Fixed

Carried over from commits merged after the v0.1.0 tag that never shipped in a release:

- **`convolutionB` not reset on Blend's disengaged->engaged transition** (#12, PR #18): unlike LoCut/HiCut/Distance, IR B's convolution engine kept no history of its own bypass state, so its internal overlap-add tail could go stale (frozen, not decaying) while Blend was disengaged and then leak into the output the moment Blend re-engaged. `CabConvolutionEngine` now tracks `blendEngagedPreviously` and calls `convolutionB.reset()` on the same disengaged->engaged transition the other stages already handle.
- **Reloading IR A after IR B silently invalidated IR B's phase alignment** (#13, PR #18): `setImpulseResponse()`/`loadDefaultImpulseResponse()` recorded IR A's new onset as the phase-alignment reference but never re-ran IR B's alignment against it, leaving an already-loaded IR B aligned to a stale, overwritten onset (reintroducing comb-filtering on the next Blend crossfade). `CabConvolutionEngine` now retains a copy of IR B's raw, pre-alignment buffer and automatically re-aligns it whenever IR A's reference onset changes.

### Other

- Housekeeping merged between v0.1.0 and this release, honestly summarised rather than omitted: branding/icon assets added and embedded in README/manual (#9, #14), a tag-triggered signed release CI workflow (#10), a marketing-copy reframe from "symphonic metal" to "heavy music" (#15), and a README fix pointing at the Releases page instead of a stale "no releases yet" note (#17).

## [0.1.0] - 2026-07-14

### Added

- Project bootstrap: README, license, contributing guide, architecture and build docs, ADRs, and CI workflow.
- DSP core: initial working Nave signal path (Convolution -> LoCut -> HiCut -> Dry/Wet Mix -> Level) with unit tests.
- **IR Blend**: a second, independently loadable impulse response slot (IR B) and an `IR Blend` parameter that crossfades between IR A and IR B (e.g. two cabs, or two mic positions on the same cab). Defaults to 0% (IR A only), bit-identical to the v0.1 single-IR signal path.
- **Inter-IR phase alignment**: loading IR B automatically time-shifts it so its transient onset lines up with IR A's, preventing comb-filtering when the two are blended together (`src/dsp/IrAlignment.{h,cpp}`).
- **Distance**: a simulated mic-to-cab distance control (post-convolution, pre-LoCut/HiCut) combining a proximity-effect low-shelf cut and a high-frequency "air absorption" high-shelf cut, both scaling with the parameter. Defaults to 0% ("off"), the same explicit-bypass-at-the-extreme pattern used by LoCut/HiCut, so the default state stays a true passthrough.
- Editor: "Load IR B..."/"Default" controls and an IR B file-name label alongside the existing IR A controls, plus IR Blend and Distance knobs.
- Broadened Catch2 test coverage: sample-rate sweep (44.1-192 kHz) null and finite-output tests, mono/stereo/unsupported bus-layout tests, long-run (2000-block and 300-block-with-loaded-IRs) NaN/Inf stability soak tests, and full unit coverage for IR Blend, Distance, and IR-onset-alignment behaviour.
- `docs/manual.md`: a full user manual (signal flow, parameter reference, usage tips).

### Deferred

- **IR browser + bundled IR library** (tracked in issue #1, left open): shipping a curated, bundled set of cabinet IRs requires either licensed real-world captures (an asset-sourcing/licensing task, not a DSP task) or synthetic placeholder IRs that could be mistaken for real captures - neither was implemented in this pass. IR Blend, Distance emulation, and inter-IR phase alignment - the DSP-engineering parts of that issue - are implemented; see the issue comment for the full rationale.
