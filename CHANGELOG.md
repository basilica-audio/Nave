# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog 1.1.0](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **`convolutionB` not reset on Blend's disengaged->engaged transition** (#12): unlike LoCut/HiCut/Distance, IR B's convolution engine kept no history of its own bypass state, so its internal overlap-add tail could go stale (frozen, not decaying) while Blend was disengaged and then leak into the output the moment Blend re-engaged. `CabConvolutionEngine` now tracks `blendEngagedPreviously` and calls `convolutionB.reset()` on the same disengaged->engaged transition the other stages already handle.
- **Reloading IR A after IR B silently invalidated IR B's phase alignment** (#13): `setImpulseResponse()`/`loadDefaultImpulseResponse()` recorded IR A's new onset as the phase-alignment reference but never re-ran IR B's alignment against it, leaving an already-loaded IR B aligned to a stale, overwritten onset (reintroducing comb-filtering on the next Blend crossfade). `CabConvolutionEngine` now retains a copy of IR B's raw, pre-alignment buffer and automatically re-aligns it whenever IR A's reference onset changes.

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
