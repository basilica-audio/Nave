# Nave — Deep-Dive Research Notes (v1 → v2)

Reference class for "guitar/bass cabinet IR loader (convolution)": **Two Notes Torpedo (Wall of Sound / C.A.B. / Torpedo Live)** — the closest thing this category has to a hardware/software de-facto standard, especially for mic-position/distance modelling; **Ignite Amps NadIR** — the free/open reference for a *pure convolution loader with tone-shaping filters*, structurally the closest existing plugin to what Nave already is; **Fractal Audio Cab block / Cab-Lab 4** — the reference for community-standard LoCut/HiCut conventions on hardware-class modelers; general **microphone-technique / reamping-engineer lore** (proximity effect physics, multi-mic blending workflow) as the grounding for what "Distance" and "IR Blend" are actually standing in for.

No hardware was measured for this pass — everything below is manual/doc/forum-sourced. See the Honesty section of the brief for what that does and doesn't license.

---

## 1. Distance / mic-proximity modelling

### Two Notes Torpedo Wall of Sound — Distance & Center are TWO separate parameters, not one

Source: [Torpedo Wall of Sound User's Manual](https://wiki.two-notes.com/doku.php?id=torpedo_wall_of_sound%3Atorpedo_wall_of_sound_user_s_manual)

> "Placing a microphone close to the cabinet will result in a precise sound with a large amount of proximity effect (depending on the chosen microphone model). When you move the microphone away from the cabinet, you increase the proportion of the studio's acoustics (early reflections) in the overall sound texture."

> "Moving the microphone away can bring some higher frequencies back. This is simply due to the directivity of the loudspeakers."

> Distance range: at maximum (100%) the simulated mic sits **~3 m (10 ft)** from the cabinet.

Two Notes' **Center** parameter (separate control) governs off-axis (lateral) placement:

> "The in-axis position (0%) allows for a maximum amount of treble sounds, which are highly directional. Moving the microphone away from the axis decreases the treble to the benefit of the bass response." Range: at 100%, mic is at the speaker edge (close) or 1 m off-axis (far).

**Gap vs. v1**: Two Notes' reference model treats "moving back" (Distance) and "moving off-axis" (Center) as two independent, physically distinct axes — distance changes proximity bass + brings some highs *back* via directivity/room reflections; off-axis position is what actually darkens the top end. Nave's single Distance knob conflates both effects into one linear-in-dB shelf pair. This is a legitimate, documented simplification (Nave's own architecture.md already says so explicitly), not an error — but the *curve shape* should still take a cue from the reference: real proximity effect is not linear in dB per % of knob travel (see §2), and it should not be "distance also darkens highs" without qualification — the reference model attributes brightening/darkening more to axis than distance.

### Proximity effect physics — corner frequency and magnitude

Source: [DPA Microphones — proximity effect explained](https://www.dpamicrophones.com/mic-university/background-knowledge/proximity-effect-in-microphones-explained/); [MyNewMicrophone — in-depth guide](https://mynewmicrophone.com/proximity-effect/); [Sweetwater InSync](https://www.sweetwater.com/insync/proximity-effect/)

- Boost concentrates **below ~200–300 Hz**, in some sources up to 500 Hz depending on mic.
- Magnitude: commonly cited **+6 dB to +20 dB** low-frequency boost at very close range (some sources cite up to +18 dB); this is directional-mic (cardioid/figure-8) physics, strongest for pressure-gradient designs.
- Distance relationship is **not linear** — the effect "accelerates exponentially once a source drops below thirty centimetres," and inverse-square amplitude falloff (quartering per doubling of distance) means the boost decays quickly with distance rather than ramping down evenly across a knob's travel.

**Implication for v2**: the reference physics is front-loaded (most of the effect happens in the first ~20–30% of "closeness"), which argues for the Distance parameter's low-shelf cut to use a *skewed/logarithmic* rather than pure-linear-in-dB taper against knob percentage — most of the perceptible bass-cut change should happen in the lower portion of the travel, tapering off at the far end, mirroring how real proximity effect saturates rather than growing forever.

## 2. Air absorption / high-frequency darkening over distance

Source: [ISO 9613-1 overview](https://www.iso.org/standard/17426.html), summarised via search (standard itself is not freely readable in full)

- Atmospheric absorption is a function of frequency, temperature, humidity, and pressure; it is **negligible below ~1 kHz and increases with frequency above that**, growing "considerably" as frequency rises.
- This confirms the general shape Nave already uses (a high-shelf *cut* that grows with distance) is directionally correct, but the standard's own coefficients only start mattering above ~1 kHz — Nave's high-shelf corner at 5 kHz is squarely in the range where real air absorption is active, which is a reasonable choice, not something to change.
- Note: at guitar-cab reamping distances (a few metres, not the tens-to-hundreds of metres ISO 9613 was written for), air absorption itself is nearly inaudible — the real-world "darkening as mic moves back" documented by Two Notes above is attributed to loudspeaker *directivity* (off-axis high-frequency rolloff), not atmospheric absorption. This matters for the *honesty section*: Nave's "air-absorption" framing in its own docs is a simplification/mislabel worth softening — the audible effect being modelled is much more speaker-directivity-driven than literal air absorption at these distances.

## 3. Reference plugin: Ignite Amps NadIR (closest structural sibling to Nave)

Sources: [NadIR v2.0.0 manual, via pdfcoffee mirror](https://pdfcoffee.com/nadir-v200-user-manual-pdf-free.html); [Bedroom Producers Blog write-up](https://bedroomproducersblog.com/2013/11/23/ignite-amps-nadir/); [Audio Plugin Guy IR loader roundup](https://www.audiopluginguy.com/apg-guide-to-guitar-cabinet-ir-loaders/)

- **HPF range: 10 Hz – 400 Hz.** (Nave's LoCut: 20–800 Hz — wider ceiling than the reference; NadIR's 400 Hz ceiling reflects that cab-IR low cut is normally a mud-control move, not a deep tonal EQ move.)
- **LPF range: 6 kHz – 22 kHz.** (Nave's HiCut: 2–20 kHz — Nave's floor of 2 kHz goes considerably lower than NadIR's 6 kHz floor; useful headroom for extreme "vintage/muffled" tones, see Fractal community lore below, but worth flagging as a deliberately wider-than-reference range in the honesty section.)
- **Resonance control**: simulates "the power amp + speaker interaction in tube amplifiers," a boost at the cab's resonant frequency; high values suit solid-state-amp IRs, lower for tone-shaping. No exact dB/Hz spec published. (Not adopted for Nave v2 — flagged as a possible M-later feature, out of scope for this brief; a resonance peak needs a dedicated parametric band, which is a bigger addition than the "sourced defaults" scope of this pass.)
- **Delay control**: 0 (default) to 20 ms, "to emulate phase interactions between multiple microphones at different distances," also usable for stereo widening. This is the direct structural analogue of what Nave's own inter-IR phase alignment (`IrAlignment::alignOnsetToReference`) automates — NadIR exposes it as a manual knob, Nave automates it. This is a legitimate, defensible design divergence (Nave's docs already justify it), not a gap.
- **Balance control**: crossfades between two loaded IRs — direct analogue of Nave's IR Blend.
- **Room control**: adds a short reverb tail "to restore room character lost when IRs are truncated" — a feature Nave does not have. Out of scope for this brief (would require a convolution/reverb addition, not a parameter-tuning pass).
- Default gain 0 dB, default delay 0 ms — i.e. NadIR's own "off" defaults mirror Nave's own default-is-transparent-passthrough philosophy.

## 4. Community-standard LoCut/HiCut conventions (Fractal Audio Cab block)

Source: [Fractal Audio Systems Forum — multiple threads](https://forum.fractalaudio.com/threads/thoughts-on-high-and-low-cuts-and-default-values.219736/), [Cab block wiki](https://wiki.fractalaudio.com/wiki/index.php?title=Cab_block)

> "By default the high/low cuts in the cab mixer page are 20/20,000 which means they aren't really doing anything" — **this exactly matches Nave's own bypass-at-extremes convention** (LoCut default 20 Hz = off, HiCut default 20 kHz = off). Independent validation that v1's default philosophy is industry-standard, not idiosyncratic.

> Community-recommended "tame the fizz" starting points, paraphrased from multiple threads: **~100 Hz low cut / ~5 kHz high cut** for general cleanup; **80 Hz / 8 kHz** favoured live; some engineers go as tight as **180 Hz low / 4.5 kHz high** for a darker vintage tone, noting "most real cabs have very narrow frequency responses" already baked into the IR itself.

> Also flagged repeatedly: adding cuts on top of an already-captured IR is *redundant* with what the mic/cab response already did — cutting is a mixing/taste decision, not a "correctness" one. Reinforces that Nave's filters are a deliberate general-purpose utility, not part of "accurately voicing" the cab itself — this belongs in the honesty section and the manual, not as a change to defaults.

**Implication for v2**: adopt "~100 Hz LoCut / ~5 kHz HiCut" and "80 Hz / 8 kHz" as sourced starting points for two of the factory presets (a "Tame the Fizz" style preset and a live-oriented preset), not as new defaults — the *default* (fully off) is already correct and independently validated above.

## 5. IR loading level/normalisation behaviour — what `Normalise::yes` actually does

Source: [JUCE forum — "Convolution normalisation factor"](https://forum.juce.com/t/convolution-normalisation-factor/44960)

> `normalizationFactor = 0.125f / sqrt(sumOfSquaredMagnitudes)`, i.e. JUCE normalises each loaded IR to a fixed total-energy target, using an admittedly "arbitrary" 0.125 constant (acknowledged by a JUCE team member on the thread as "a very good choice to start with," not a precisely justified reference level).

> Consequence: because the factor is driven by *total energy*, two real-world cab IRs of different length/spectral content/reverb tail can still land at meaningfully different perceived loudness after JUCE's own normalisation — the thread explicitly notes gains from the same nominal setup can vary by tens of dB depending on spectral density of the source IR (dense reverb IR vs. sparse impulse behave very differently under the same energy-based normalisation).

**Gap vs. v1**: Nave calls `Convolution::Normalise::yes` for every user-loaded IR (both slots) and currently offers no messaging that this is an *energy-normalisation*, not a *perceptual loudness match* — two real-world cab IRs captured at different mic distances/gain levels can land at audibly different output levels after loading, with only the manual Level trim as recourse. This is worth an explicit callout in the manual/honesty section (not a DSP change — replacing JUCE's normalisation with a custom LUFS-style match is out of scope for a v0.2.0 parameter-tuning pass) and a factory-preset-adjacent tip.

## 6. IR blending workflow lore (close mic + room mic, blend ratios)

Sources: [Overdriven.fr — "Impulse responses part 9: mixing IRs"](https://overdriven.fr/overdriven/index.php/2021/05/30/impulse-responses-part-9-mixing-irs/); [Fractal Audio Forum — reamping/multi-mic thread](https://forum.fractalaudio.com/threads/reamping-and-multiple-mics-on-a-cab-questions.52841/)

- Common engineer practice: close-mic the cab (often a dynamic, e.g. SM57-style, for edge/attack), place a second mic (ribbon or condenser) further back for room/body, blend to taste.
- Cited blend ratios in circulation: **75/25, 50/50, 25/75, 10/90** — i.e. discrete, musically meaningful stopping points rather than a continuous sweep being explored uniformly.
- Phase caution: if blending two mic signals "makes the sound smaller," that's the signature of a phase/timing mismatch, fixed by nudging one source into alignment — this is exactly the problem Nave's `IrAlignment::alignOnsetToReference()` automates away, and worth stating plainly as the payoff of that feature in a preset's description text.

**Implication for v2**: factory presets built around IR Blend should target the sourced discrete ratios (10–25% for "add a touch of room/second mic," 50% for an even blend) rather than arbitrary numbers, and preset copy can legitimately claim "the same discrete blend ratios engineers reach for when compositing close + room mics, without the manual phase-alignment work."

---

## Summary: what v1 gets right vs. what's generic/missing

**Already correct / independently validated by research** (do not change):
- Bypass-at-extremes convention (20 Hz LoCut / 20 kHz HiCut off) — matches Fractal's own default convention exactly.
- Proximity-effect corner frequency choice (~200 Hz low shelf) is inside the physically documented 200–300 Hz range.
- High-shelf darkening for "distance" is directionally correct (frequency-dependent attenuation growing with distance/off-axis is real), even if the literal "air absorption" framing is a mislabel (see §2).
- Zero-latency convolution choice is architecturally sound and matches the reamping/tracking latency-sensitivity rationale used industry-wide.

**Generic / under-sourced, worth revising in v2:**
- Distance's linear-in-dB-per-% taper doesn't reflect the front-loaded, non-linear physical curve of real proximity effect (§1–2).
- "Air absorption" framing in docs/manual overstates a physically negligible effect at these distances vs. the actual (directivity-driven) cause — honesty/manual wording issue, not a DSP bug.
- No factory presets yet exist (M2 is explicitly deferred) — this pass proposes sourced starting points.
- No user-facing acknowledgement that loading two different real-world IRs can produce different post-load loudness due to JUCE's energy-based (not perceptual) normalisation (§5).
- LoCut/HiCut ranges are wider than the closest structural reference (NadIR: 10–400 Hz / 6–22 kHz) at the low-cut ceiling and high-cut floor; Nave's wider range (20–800 Hz / 2–20 kHz) is defensible (more headroom for extreme tones) but should be stated as a deliberate divergence, not treated as unexamined.
