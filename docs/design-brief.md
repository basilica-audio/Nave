# Nave — Design Brief v2 (target: v0.2.0)

Status: draft, for review. Supersedes the implicit v1 "brief" embedded in `docs/architecture.md`/`docs/manual.md`. Research basis: `nave-research-notes.md` (same directory) — manual/forum/standards-sourced, no hardware measured. See **Honesty** section before treating any number below as gospel.

## Why v1 falls short of the reference class

Nave v1 (M1, "DSP completion & test coverage") is an **engineered core**: the convolution/blend/filter/mix signal chain is architecturally sound, real-time-safe, and thoroughly null-tested. But its tonal-control defaults were derived from internal reasoning ("linearly scale a shelf in dB against a percentage knob," "pick round numbers for filter ranges") rather than from how the reference class — Two Notes Torpedo, Ignite Amps NadIR, Fractal Audio's Cab block, and the underlying microphone physics all three are modelling — actually behaves. Concretely:

1. **Distance conflates two physically distinct effects into one linear taper.** The reference class (Two Notes) treats "how far back" and "how far off-axis" as two separate control axes because they *are* physically separate (proximity effect vs. loudspeaker directivity). Nave's single knob approximates both at once with a plain linear-in-dB shelf pair — defensible as a simplification, but the taper shape doesn't reflect that real proximity effect is front-loaded (most of the audible change happens early in the travel, then it saturates), not evenly spread across 0–100%.
2. **The "air absorption" framing is a mislabel.** At cab-to-mic reamping distances (well under Two Notes' own 3 m Distance ceiling), literal atmospheric absorption is close to inaudible; the real high-frequency loss Two Notes documents is loudspeaker directivity (off-axis rolloff), not air. Nave's docs/manual currently claim the wrong physical cause for a real, correctly-shaped effect.
3. **No factory presets exist**, and M1's own filter/blend ranges were never checked against a structural sibling plugin. NadIR (the closest existing plugin to what Nave already is) publishes HPF 10–400 Hz / LPF 6–22 kHz; Nave's 20–800 Hz / 2–20 kHz is wider at both open ends without that being a stated, deliberate choice anywhere in the docs.
4. **No user-facing acknowledgement of a real gotcha**: JUCE's `Convolution::Normalise::yes` is an energy-normalisation, not a loudness match — loading two different real-world cab IRs can land at different post-load output levels, and nothing in Nave's UI/manual currently prepares a user for that or points them at Level as the fix.
5. **IR Blend has no sourced target ratios.** The reference workflow (close mic + room mic) reaches for a small number of discrete, well-known ratios (10–25%, 50%, 75/25, etc.), not a uniform sweep — Nave's manual gestures at "10–25%" once but nothing else in the product reflects this.

None of this is a DSP bug — the M1 engine (bypass-at-extremes, phase alignment, zero-latency convolution, real-time safety) holds up and is *independently validated* by the research (Fractal's own 20/20,000 "off" convention matches Nave's bypass defaults exactly). v2 is a **tonal-authenticity pass on the existing topology**, not a re-architecture.

## Topology (unchanged from v1)

```
Input --> Convolution (crossfade IR A / IR B) --> Distance --> LoCut (HPF) --> HiCut (LPF)
                                                                                      |
                              Output <-- Level (output trim) <-- Mix <---------------+
                                                                    ^
                                                                    |
                                                        delay-compensated dry path
```

No new blocks, no new latency, no change to the convolution/phase-alignment/bypass-at-extremes architecture. Every change below is a parameter range, taper, or default revision inside the existing chain, plus new preset data (M2-facing) and doc/UI copy corrections — deliberately scoped to stay a fast-follow rather than a re-architecture, consistent with "breaking parameter changes allowed pre-1.0."

## Module specs (sourced defaults)

### Distance (mic-to-cab proximity + off-axis coloration)

- **Range**: 0–100%, unchanged.
- **Default**: 0% (off), unchanged — independently correct; a cab IR loader's default state must be a true passthrough (already covered by the bypass-at-extremes test suite).
- **Taper — CHANGE**: replace the current plain-linear-in-% dB scaling with a **skewed taper** (e.g. `normalisedDistance^1.6`–`^2` applied before the existing linear-in-dB shelf-gain scaling, tuned by ear against the sourced curve shape, not derived analytically) so the low-shelf cut (proximity) is front-loaded — most of the audible change happens in the first third of the knob's travel, tapering off toward 100% — mirroring the documented "accelerates exponentially at close range, then saturates" physical curve (research notes §1). The high-shelf (directivity/darkening) side can keep a gentler, closer-to-linear taper since Two Notes explicitly separates "high end returns as you move back" (Distance) from "high end drops as you go off-axis" (their Center) — Nave's single knob is intentionally modelling something closer to the *darkening* half of that story for its high-shelf, so a more linear taper there is the honest simplification, not an oversight.
- **Corner frequencies — unchanged**: low-shelf ~200 Hz (inside the sourced 200–300 Hz proximity-effect range), high-shelf ~5 kHz (inside the sourced "air absorption/directivity becomes audible above ~1 kHz, increasingly so higher" range).
- **Max cut magnitude**: keep existing max-cut dB constants unless listening confirms the new taper needs rebalancing to avoid over-darkening at 100% — this is a taper-shape change, not a range change, so the existing `distanceLowShelfMaxCutDb`/`distanceHighShelfMaxCutDb` ceiling values are a reasonable starting point to re-audition, not to blindly keep.
- **Naming**: keep "Distance" (generic, already correct — no brand names anywhere in v1 or this brief).

### LoCut (post-convolution high-pass)

- **Range — CHANGE**: keep 20–800 Hz. *(Considered narrowing to NadIR's 400 Hz ceiling; rejected — Nave's wider ceiling gives headroom for deliberately extreme "telephone/lo-fi" tones some engineers do reach for, and narrowing is a pure regression with no sourced justification for removing headroom. Documented here as a deliberate keep, closing the "unexamined" gap from the research notes.)*
- **Default**: 20 Hz (off), unchanged — matches Fractal's own 20 Hz "off" convention exactly (sourced, §4).
- **Taper**: unchanged (existing log-frequency `NormalisableRange`, correct for a frequency control).

### HiCut (post-convolution low-pass)

- **Range**: keep 2 kHz – 20 kHz, same reasoning as LoCut (deliberate keep, wider floor than NadIR's 6 kHz for extreme dark/vintage tones referenced in Fractal community lore, §4).
- **Default**: 20 kHz (off), unchanged — matches Fractal's 20,000 Hz "off" convention exactly.
- **Taper**: unchanged.

### IR Blend

- **Range/default**: unchanged (0–100%, default 0%).
- **CHANGE — no new parameter, but new preset-facing guidance**: expose the sourced discrete blend ratios (10%, 25%, 50%, 75%) as the values factory presets actually land on, rather than presets landing on arbitrary numbers. No DSP or range change; this is purely a defaults-for-presets decision informed by §6.

### Mix / Level

- Unchanged. Both are already at industry-standard defaults (100% wet — cab IR is normally fully in-chain; 0 dB trim) with no sourced reference class disagreement found.

### New: manual/UI copy corrections (not a parameter change, but part of "authentically voiced")

- Replace "air-absorption darkening" framing in README/manual/architecture.md with a directivity-first explanation ("as a mic moves off-axis and further back, a real cabinet's high end rolls off — this models that darkening; it's driven far more by speaker directivity than by literal air absorption at these distances").
- Add one sentence to the manual's IR-loading section noting that two different loaded IRs can land at different output levels because loading normalises each IR's *energy*, not its perceived loudness — pointing at Level as the fix. Sourced from the JUCE-forum-documented `0.125f / sqrt(energy)` normalisation behaviour (§5) — no DSP change, just setting correct user expectations.

## Factory Presets (proposed set for the M2 preset system, 8 presets)

All values below are **starting points to audition against the new Distance taper**, not final locked numbers — treat as the brief's proposal, confirm by ear once the taper change lands.

1. **Default / Transparent** — intent: the certified passthrough state, exposed as an explicit preset so users always have a one-click way back to "no coloration." LoCut 20 Hz (off), HiCut 20 kHz (off), Distance 0%, Blend 0%, Mix 100%, Level 0 dB.
2. **Tame the Fizz** — intent: general-purpose high-gain cleanup, sourced from Fractal community consensus (§4). LoCut ~100 Hz, HiCut ~5 kHz, Distance 0%, Blend 0%, Mix 100%, Level 0 dB.
3. **Live Stage** — intent: tighter, more aggressive cut for a live monitoring/tracking chain where mud and fizz both cost headroom, sourced from the "80/8000 for live" community datapoint (§4). LoCut ~80 Hz, HiCut ~8 kHz, Distance 0%, Blend 0%, Mix 100%, Level 0 dB.
4. **Dark Vintage** — intent: darker, narrower-band tone for a vintage/lo-fi cab character, sourced from the "180 Hz/4.5 kHz" community datapoint (§4). LoCut ~180 Hz, HiCut ~4.5 kHz, Distance ~25% (light proximity push), Blend 0%, Mix 100%, Level 0 dB.
5. **Pushed Back in the Room** — intent: showcase Distance alone as the sourced "finishing touch" it's documented to be. LoCut 20 Hz (off), HiCut 20 kHz (off), Distance ~60%, Blend 0%, Mix 100%, Level +1 to +2 dB (to compensate for shelving cuts, per the manual's own "check Level" tip).
6. **Touch of Room Mic** — intent: showcase IR Blend at the sourced low-ratio end (§6) — requires IR B loaded by the user; preset only sets the knob. LoCut 20 Hz (off), HiCut 20 kHz (off), Distance 0%, Blend 15%, Mix 100%, Level 0 dB.
7. **Even Blend** — intent: the sourced 50/50 discrete stopping point (§6) for two genuinely complementary IRs (two cabs, or close+room). Blend 50%, everything else at defaults.
8. **Parallel Cab (Blended Dry)** — intent: showcase Mix as a genuine parallel-processing tool (not just "on/off wet"), pairing a moderate Distance push with a partial Mix for a thickened, less "all-or-nothing" cab tone. LoCut 20 Hz (off), HiCut 20 kHz (off), Distance ~20%, Blend 0%, Mix ~65%, Level +1 dB.

Presets 1–4 are single-IR-slot-safe (never require IR B); 6–7 are explicitly Blend-focused and should carry UI copy noting they need an IR loaded into slot B to be audible (mirroring the manual's own existing warning that Blend "has no audible effect unless an IR is loaded into slot B").

## Catch2 test guarantees (additions for v2)

Existing suite (50 tests, `tests/*.cpp`) already covers: bypass-at-extremes null tests, IR Blend crossfade math (including the "must convolve the same dry input, not cascade" ordering), inter-IR phase alignment, latency reporting, state round-trip, NaN/Inf robustness. v2 adds:

- **Distance taper monotonicity + shape**: for a swept set of Distance values (e.g. 0/10/25/50/75/100%), assert the low-shelf cut magnitude is (a) monotonically non-decreasing, (b) covers *more* of its total cut range in the first half of the sweep than the second half (a measurable proxy for "front-loaded" — e.g. `cutAt50Percent - cutAt0Percent > cutAt100Percent - cutAt50Percent`), directly encoding the "front-loaded, not linear" requirement from the brief rather than just re-testing the existing linear behaviour.
- **Distance taper regression guard**: a fixed-point snapshot test (Distance at 25/50/75/100%, tolerance-banded expected shelf gains in dB) so a future accidental taper-curve regression fails loudly, the same pattern `EngineTests.cpp` already uses for bypass epsilon checks.
- **LoCut/HiCut range-unchanged guard**: assert `loCutMinHz == 20`, `loCutMaxHz == 800`, `hiCutMinHz == 2000`, `hiCutMaxHz == 20000` as an explicit, named test (`"LoCut/HiCut ranges match the v2 brief's deliberate-keep decision"`) — cheap insurance against silent range drift now that the brief has explicitly documented *why* these differ from the NadIR reference rather than leaving that reasoning only in a markdown file nobody re-reads before touching `CabConvolutionEngine.h`'s constants.
- **Preset-value smoke test** (once M2's preset system lands): for each of the 8 proposed presets, assert `process()` produces finite, non-NaN, non-clipping (peak below some sane ceiling, e.g. +6 dBFS on a known test signal) output — a coverage-style guard, not a golden-audio comparison.
- **Normalisation-awareness regression note**: not a new automated test (there's no way to unit-test "is this perceptually loud-matched" without reference audio), but add a comment in `EngineTests.cpp` near the IR-loading tests cross-referencing the manual's new normalisation callout, so a future contributor investigating a "why do these two IRs sound different loudness" bug report finds the documented, sourced explanation instead of re-discovering it.

## Honesty

Every number and curve-shape decision in this brief is **research-derived from manuals, forum measurements, physics standards summaries, and one other plugin's published spec sheet — not from measuring real hardware, real cabinet IRs, or real Two Notes/Fractal/NadIR units.** Specifically:

- The Two Notes Distance/Center split, its 3 m/1 m ranges, and its qualitative behaviour description come from Two Notes' own manual text (paraphrased/quoted), not from operating a physical Torpedo unit or capturing IRs at varying documented mic distances and comparing curves.
- The proximity-effect corner frequency and magnitude figures (200–300 Hz, +6 to +20 dB) are **generic microphone-physics figures** from audio-engineering reference sites, not measurements of any specific mic/cab pairing Nave's cabinet IRs will actually be loaded with — real proximity effect varies significantly by microphone polar pattern and model, which Nave (correctly, per its own architecture docs) does not attempt to model per-mic.
- The proposed "front-loaded" Distance taper exponent (`^1.6`–`^2`) is a **reasoned approximation of a qualitative physical description** ("accelerates exponentially... then saturates"), not a curve fit to measured data — it should be tuned by ear during implementation, and the Catch2 "shape" test above is deliberately a *coarse* monotonicity/front-loading check, not a tolerance-banded match to a specific published curve, precisely because no such curve was sourced.
- The "air absorption is a mislabel, directivity is the real cause" claim is inferred by cross-referencing two separate sources (ISO 9613's frequency threshold + Two Notes' own explicit attribution to loudspeaker directivity), not stated together in any single primary source.
- The LoCut/HiCut community-lore preset frequencies (100/5k, 80/8k, 180/4.5k) are paraphrased from Fractal Audio forum threads — user opinion and anecdote, explicitly not a spec, and explicitly acknowledged in those same threads as a matter of taste rather than correctness.
- JUCE's `Normalise::yes` normalisation-factor formula is sourced from a JUCE forum post by a community member (not Anthropic-verified against JUCE 8.0.14 source directly in this pass) — worth a source-code cross-check (`juce_Convolution.cpp`) before the manual copy ships, flagged here rather than silently assumed correct.

**What this licenses**: confidently revising Nave's *taper shapes*, *default philosophy documentation*, *preset starting points*, and *doc/manual wording* to be more defensible and better-sourced than v1's internally-reasoned numbers. **What it does not license**: claiming Nave now "sounds like" any specific reference hardware, or treating the taper exponent / preset frequencies as precision-calibrated rather than reasoned-and-auditioned starting points. Any marketing copy building on this brief must not claim hardware-matched accuracy — this is engineering-documentation-derived plausibility, not measurement.

## Versioning

- **Target**: v0.2.0 (SemVer, pre-1.0 — breaking parameter changes are explicitly allowed per the project's own roadmap/status).
- **Breaking changes in this brief**: the Distance taper shape change is parameter-*behavior*-breaking (same range/default, different curve) — a saved session with Distance at, say, 60% will sound different after this ships. This is acceptable pre-1.0 per project convention, but should be called out plainly in `CHANGELOG.md` under a "Changed" heading, not buried.
- **State migration**: tolerant import — `AudioProcessorValueTreeState`'s existing round-trip (including the non-APVTS `irFilePathProperty`/`irFilePathBProperty` tree properties) already handles missing/extra parameters gracefully; no new migration code needed since no parameter IDs, ranges, or defaults change — only the internal taper curve applied to the existing Distance value changes, which is inherently forward/backward compatible with old saved states (a stored Distance value of 60.0 remains meaningful, just mapped through a new curve).
- **No M2/M3 scope creep**: this brief does not add GUI, does not add a preset *system* (that's M2's own scope) — it only supplies the *content* (8 preset value sets) M2's preset system will need once it exists, and the parameter/taper/doc changes M2 doesn't block on.
