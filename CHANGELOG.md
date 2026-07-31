# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.3.1] - 2026-07-31

Crash-fix patch release.

### Fixed

- **Cross-thread race in `CabConvolutionEngine` crashing hosts during automation** (#27). The engine's message-thread-only methods had two unsynchronized entry points - the host's `prepareToPlay()` thread and JUCE's async-updater message thread - which could concurrently call `juce::dsp::Convolution::loadImpulseResponse()`. Its background queue push is only safe from a single thread at a time; concurrent callers corrupted the queued command slots and the loader thread crashed with `std::bad_function_call` (reproduced twice in CI at 96 kHz under pluginval's Automation test). All message-thread-only methods are now serialized behind a mutex that the audio thread never touches - no realtime-safety or behavioral change. Red-verified: the unfixed engine reproduces the crash under a cross-thread reprepare/automation stress test; the fixed engine survives 30+ runs clean (`tests/CrossThreadReprepareTests.cpp`).

### Documentation

- New "Under the hood" engineering section, explicit latency statement, and known-limitations section in the manual; corrected the zero-latency claim to distinguish the stock JUCE engine (Crossfade) from the custom morph convolver path.

## [0.3.0] - 2026-07-27

"First-class cab engine". Nave becomes the only open IR loader whose two-IR blend is a true mic-position morph - comb-free at every blend value - and closes the gaps that kept it from being a flagship: sessions now embed their IR audio instead of only a file path, IR alignment is a measurement rather than a heuristic, and IR levels can be matched by loudness rather than by raw energy.

**Every new parameter defaults to a neutral value, so a v0.2 session or preset renders bit-identically after upgrading** - pinned by golden-render null tests, including a blend-engaged session with real IRs in both slots.

### Added

- **Mic-morph IR blending** (`blendMode` = Morph). Instead of crossfading two convolvers, the morph decomposes each IR into a minimum-phase magnitude and a bulk delay, interpolates those separately, and runs the resynthesised result through a single custom convolver. Because only one IR is ever in the signal path there is nothing to comb against - the failure every crossfading loader has at Blend 50%. Measured: a 7-sample-offset IR pair combs 82 dB when summed naively; morphed at 50/50 the magnitude tracks the original within 1.0 dB (99th percentile, below 8 kHz). Interpolating log-magnitude (a geometric mean) keeps shared resonances at full level instead of dipping; interpolating the bulk delay separately makes a Blend drag glide in time like a mic physically moving. Resynthesis runs on a coalescing worker thread; the audio thread never waits for it and never takes a lock. Morph minimum-phases both endpoints by construction, so Blend 0% is the minimum-phase IR A rather than raw IR A - which is why `blendMode` defaults to Crossfade and switching is an opt-in change of character.
- **`MorphConvolver`**, a custom uniformly-partitioned overlap-save convolver whose IR can be exchanged mid-signal without a click. One shared frequency-domain delay line of input spectra feeds two filter-spectra sets; publishing runs both against that shared history and crossfades the outputs, so there is no state reset at all. `juce::dsp::Convolution` cannot do this - its `loadImpulseResponse()` is a hard engine swap. Measured: nulls against direct time-domain FIR convolution below 1e-5 at IR lengths 1/173/255/256/257/512/1024/16384, exact zero latency, chunk-size invariance at the single-precision rounding floor.
- **IR audio embedded in the session** (state schema v2). Through v0.2 a saved project stored only the IR files' absolute paths, so moving the project, handing it to a collaborator, or opening it on another machine silently replaced the cabinet with a delta IR - no error, no visible sign. The audio is now stored gzip'd in the plugin state, with paths kept alongside as display and relocation metadata; embedded audio wins on load. Measured: a session whose IR files are deleted after saving reproduces the original render below -80 dBFS. IRs longer than 10 s per slot stay path-only.
- **Precise IR alignment** (`alignMode` = Precise, the default for new instances). Replaces the 20%-of-peak onset heuristic with FFT cross-correlation, sub-sample peak refinement, fractional-delay application, and automatic polarity detection. The heuristic fails exactly where it matters most - two captures of one cabinet whose transients rise at different rates cross a relative threshold at different points, so the "aligned" result still combs. Measured: offsets of 7, 37.25 and 0.5 samples all recovered within 0.1 samples; an inverted, delayed capture is detected and corrected, taking the 50/50 blend from -5.8 dB of cancellation to 0.0 dB against the ideal.
- **Loudness-matched IR normalisation** (`irGainMode` = Loudness). Scales each IR by its ITU-R BS.1770 K-weighted energy instead of raw energy, so A/B-ing two IRs compares tone rather than level. Measured: a bright short capture and a dark long one land within 0.5 dB of each other under white-noise excitation, against more than 1 dB apart under Energy mode.
- **Per-slot IR B controls**: `irBTrim` (-24 to +24 dB), `irBPolarity`, and `irBDelay` (+/-5 ms). The delay is dual-sided - the B branch delays by max(d, 0) and the A branch by max(-d, 0) - so both delay times are non-negative, both processors are skipped entirely at 0 ms, and the mapping stays continuous through zero.
- **Per-slot minimum-phase transform** (`irAMinPhase`, `irBMinPhase`), for mixing IRs captured by different vendors without phase cancellation. Never destructive: the raw IR is retained, so switching back restores the original exactly.
- **Distance Air** (`distanceAir`): the time-of-flight component of mic distance, 2.9 ms at Distance 100%, on top of the existing tonal model. Measured within 0.02 ms at 25/50/100%. Off by default.
- **24 dB/oct LoCut and HiCut** (`loCutSlope`, `hiCutSlope`). Measured: -24.40 dB one octave below a 400 Hz LoCut at 24 dB/oct, -12.42 dB at 12 dB/oct. Switching slope crossfades two independently primed filter chains over 10 ms.
- **Mono-in/stereo-out bus support**, so a mono DI fills a stereo bus instead of playing out of one side.
- **Two new factory presets**: "Mic Morph" and "Tight Stack".
- **New test suites**: `MorphConvolverTests`, `MorphEngineTests`, `MinPhaseTests`, `IrStateTests`, and `AllocationGuardTests` - the last replacing the global allocator to prove `process()` allocates nothing with every feature engaged.

### Changed

- **`getTailLengthSeconds()` now reports the longer loaded IR** instead of 0. Reporting 0 told the host the plugin had no tail, so bounce, freeze and bypass operations truncated a decaying cabinet the moment the source stopped.
- **Filter coefficient updates are allocation-free and run every 32 samples** rather than once per block. v0.2's `*state = *IIR::Coefficients<float>::make...` heap-allocated a ref-counted object on the audio thread on every engaged block; `ArrayCoefficients` returns a plain `std::array` by value and is assigned into storage allocated once in `prepare()`. Verified: restoring the v0.2 idiom produces 512 audio-thread allocations in the guard's measurement window, against zero now.
- **Blend, Mix, IR B Trim and IR B Polarity gains advance per sample** rather than being stepped once per block, removing zipper noise on fast moves at large block sizes. Static settings are unchanged.
- **Editor** gains the new controls in the existing plain style. The custom look-and-feel remains a later milestone.

### Fixed

- **`FractionalDelay` was indexing its four-tap Lagrange window one sample early**, so every delay in the engine was short by exactly one sample. Found by the Distance Air measurement.

### Notes on behaviour

- **Three switches briefly reset the convolution engine** when changed: IR Gain Match, the per-slot Min-Phase toggles, and IR Align. `juce::dsp::Convolution` has no crossfade hook - loading an IR resets it - and the dual-instance machinery that would hide this is deliberately scoped to the morph path only in this release. These are rare, stepped configuration changes, and v0.2 already reset audibly on every IR load. Click-free IR exchange is guaranteed on the morph path; every continuous control (Blend, Mix, Trim, Polarity, Delay, Distance, and the slope switch) stays click-free.
- **Loudness matching is exact for spectrally flat material and approximate for coloured program material**, since K-weighted IR normalisation equalises the weighted IR energy rather than the weighted output level.


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
