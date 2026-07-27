<p align="center"><img src="assets/icon.png" alt="Nave icon" width="120"/></p>

# Nave user manual

*Cabinet impulse-response loader for guitar and bass reamping.*

## What Nave is

Nave takes a dry, un-amped instrument signal (a DI guitar or bass track, or the pre-cab output of an amp sim) and convolves it with the impulse response ("IR") of a real (or emulated) speaker cabinet and microphone. In other words: Nave is where a dry, buzzy DI signal becomes something that sounds like it was mic'd off a real cab in a room.

In a heavy production chain, Nave typically sits **after** distortion/amp-sim processing and **before** EQ/bus processing:

```
DI guitar/bass -> amp sim / preamp distortion -> Nave (cab IR) -> EQ / compression -> mix bus
```

It's equally at home reamping a recorded DI track after the fact, or running live in a monitoring chain while tracking.

## Signal flow

```
Input --> Convolution (crossfade of IR A / IR B) --> Distance --> LoCut (HPF) --> HiCut (LPF)
                                                                                          |
                                    Output <-- Level (output trim) <-- Mix <--------------+
                                                                          ^
                                                                          |
                                                              delay-compensated dry path
```

1. **Convolution.** Your instrument signal is convolved with the loaded impulse response(s). With no IR loaded, Nave runs a mathematically transparent unit-impulse ("delta") IR — it's a valid, silent-by-default effect out of the box, not a placeholder that colours your sound until you load something.
2. **Distance.** An optional, simulated mic-distance coloration (see [Distance](#distance-simulated-mic-distance) below). Off by default.
3. **LoCut / HiCut.** Two general-purpose tone-shaping filters for cleaning up the convolved signal — a high-pass to tighten the low end, a low-pass to tame fizz/harshness. Both are off by default (wide open).
4. **Mix.** Blends the fully-processed ("wet") signal back with your original dry input. Defaults to 100% wet — a cab IR is normally run fully in the chain, not blended with the raw DI.
5. **Level.** A final output trim, so switching cabs/settings doesn't also throw off your downstream gain staging.

See [`architecture.md`](architecture.md) for the implementation-level details (latency handling, filter-bypass semantics, IR file state).

## Loading impulse responses

Nave has **two independent IR slots**, A and B:

- **IR A** — the primary/original slot. Use the **Load IR...** button to pick a `.wav`/`.aiff` cabinet IR file; **Default** clears it back to the built-in transparent delta IR.
- **IR B** — a secondary slot, loaded and cleared the same way via **Load IR B...** / **Default**. On its own it does nothing (see [IR Blend](#ir-blend) below) — it only matters once you dial in some Blend.

**Your IR audio is saved inside the session** (new in v0.3.0). Up to 10 seconds per slot of the loaded IR is stored in the plugin's own state, so a project reopens with the same cabinets even if the original files have been moved, renamed, deleted, or left on another machine. The file paths are still saved alongside, so the editor can tell you where an IR came from — but the sound no longer depends on them. (Before v0.3.0 only the path was saved, and a missing file silently reverted the slot to the transparent default. If you have older projects, reopening and re-saving them in v0.3.0 makes them self-contained.) An IR longer than 10 seconds is still stored path-only, since a cabinet IR is never that long and embedding one would bloat your session file.

### IR Gain Match

Two loaded IRs can sound noticeably different in level even at identical settings. **IR Gain Match** decides how Nave levels them:

- **Energy** (default) matches each IR's raw energy. This is what Nave has always done, and it is what most IR loaders do.
- **Loudness** matches each IR's *K-weighted* energy — the same weighting a LUFS meter uses, which discounts the sub-bass and boosts the presence range roughly the way your ear does. A dark 4x12 capture carries far more of its energy where the ear is least sensitive, so Energy mode leaves it audibly quieter than a bright close-mic capture even though the two measure the same. In Loudness mode, switching IRs changes the tone without lurching in level, so an A/B actually compares tone.

Loudness matching is exact for spectrally flat material and approximate for real program material, since it equalises the weighted energy of the *impulse response* rather than of your particular guitar take. Either way, **Level** remains the place to make a final adjustment.

Changing this control briefly resets the convolution engine, so set it while the transport is stopped if you are being fussy — see [A note on the three "reset" switches](#a-note-on-the-three-reset-switches).

### IR Align

When you load IR B, Nave time-aligns it against IR A so that blending the two does not comb-filter. **IR Align** chooses how:

- **Precise** (default for new instances) cross-correlates the two IRs, finds the offset to a fraction of a sample, and also detects when IR B is polarity-inverted relative to IR A — a rear-of-cabinet mic, or a mispatched preamp — and flips it for you. Without that flip, blending the two would partially cancel instead of adding.
- **Legacy** is the simpler onset-detection method Nave used through v0.2. Sessions saved before v0.3.0 are automatically set to Legacy when you open them, so they keep sounding exactly as they did. Switch to Precise if you want the improvement.

### Min-Phase (per slot)

**IR A Min-Phase** and **IR B Min-Phase** convert a slot's IR to its minimum-phase equivalent: identical frequency response, but with all the excess phase removed and the energy pulled to the front. This is what makes IRs from different sources mix cleanly — two captures that sound fine alone can cancel each other in the blend purely because of phase, and minimum-phasing both removes that variable.

It is never destructive. Nave keeps the original IR, so switching the toggle back restores it exactly.

### IR Blend

The **IR Blend** knob moves between IR A (0%) and IR B (100%). Typical uses:

- **Two different cabs** — blend a tight 4x12 with a boomier 2x12 to taste, without needing a separate blending plugin.
- **Two mic positions on the same cab** — e.g. an on-axis close mic (IR A) blended with a room/ambient mic (IR B) for more dimension.

When you load IR B, Nave automatically **phase-aligns** it to IR A's transient onset before the two are ever mixed together. Two real-world IR captures rarely start at exactly the same moment (different mic distances, different capture setups), and blending misaligned IRs directly would partially cancel a wide band of frequencies (comb filtering) — the alignment step prevents that, so IR Blend sounds like a genuine tonal blend rather than a phasey mess.

Blend defaults to 0% (IR A only) — loading an IR B and leaving Blend at 0% has no audible effect until you turn the knob up.

### Blend Mode: Crossfade or Morph

**Crossfade** (default) runs both IRs and fades between their outputs. It is predictable and it is what Nave has always done — but at intermediate settings you are hearing *two* cabinets at once, and wherever their direct sounds arrive at slightly different times they partially cancel. That is comb filtering, and it is worst at 50%, which is exactly where "somewhere between these two mics" lives.

**Morph** is the alternative, and it is what this release is built around. Instead of summing two IRs, Nave takes them apart — separating each one into *what it sounds like* (its frequency response) and *when it arrives* (its timing) — interpolates those two things independently, and rebuilds a single new impulse response from the result. There is only ever one cabinet in the signal path, so there is nothing to comb against. Dragging Blend sweeps continuously between the two captures the way physically moving the mic would, including the subtle pitch glide of a mic in motion.

Two things to know:

- **Morph changes the endpoints too.** At Blend 0% you hear the minimum-phase version of IR A, not IR A exactly. That is inherent to the technique, and it is why Crossfade remains the default — switching to Morph is a deliberate change of character, never something a session acquires on its own.
- **Morph shines on related captures.** Two mic positions on the same cabinet morph beautifully. Two completely unrelated cabinets will morph through magnitude responses that no real cabinet has; that can be a useful sound, but it is sound design rather than mic placement.

### IR B Trim, Polarity and Delay

Three controls that act on the IR B branch only, for dialling in a dual-mic blend the way an engineer would at the console:

- **IR B Trim** (-24 to +24 dB) balances slot B against slot A without touching the overall output.
- **IR B Polarity** flips slot B. Nave's Precise alignment already corrects an inverted capture automatically, so this is the manual override for when the "wrong" polarity is the sound you want.
- **IR B Delay** (+/-5 ms) offsets slot B in time. Positive values push slot B later; negative values pull it earlier, which — since nothing can arrive before it was played — is realised by delaying slot A by the same amount instead. The two are the same relative offset, but at a negative setting the whole wet path sits |d| milliseconds later in absolute terms. At exactly 0 ms neither branch is delayed at all, so the default costs nothing and changes nothing. Small offsets here are the classic trick for thickening a doubled cab sound, or for deliberately dialling in the comb notch that alignment removes.

### Distance Air

The Distance knob's tonal model does not change *when* sound arrives — but moving a mic back genuinely does, at roughly 2.9 ms per metre. **Distance Air** adds that time-of-flight delay to the wet path, so pulling Distance back also pushes the cabinet back in time. Off by default; at Distance 0% it does nothing regardless.

Automating Distance with Air on glides the delay rather than stepping it, which is the correct Doppler behaviour for a moving mic — and, incidentally, a nice effect in its own right.

### LoCut and HiCut slopes

Both filters can now run at **12 dB/oct** (default, and what v0.2 shipped) or **24 dB/oct**. The steeper setting gets out of the way faster, which is useful when you want to remove low-end mud without thinning the body just above it. Switching slope crossfades the two filters over 10 ms, so it is silent even mid-take.

### Distance (simulated mic distance)

The **Distance** knob is a simplified emulation of moving the mic further from the cab: at higher settings it reduces low-end proximity buildup and dulls the top end slightly. The top-end darkening is modelled as a real cabinet's high end rolling off as a mic moves further back and off-axis — that's driven far more by loudspeaker directivity than by literal air absorption at typical reamping distances, so don't read it as "the air between the mic and the cab" so much as "how the speaker itself radiates less high end off to the side." It is *not* a physically exact distance model — no pre-delay/timing change is applied — just a musically useful tonal shift for pushing a too-close/too-bright IR back in the mix, without reaching for a separate EQ. The low end responds faster near the start of the knob's travel and tapers off toward 100%, mirroring how real proximity effect behaves — most of the change happens early, not spread evenly across the full sweep.

Distance defaults to 0% ("off" — no coloration applied at all, a true passthrough at this stage of the chain).

## Parameter reference

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| **LoCut** | 20 – 800 | 20 (off) | Hz | Post-convolution high-pass filter. At its minimum (20 Hz, the default) it's fully bypassed — a true passthrough, not just an inaudible cutoff. Raise it to tighten a boomy cab IR or tame low-end mud before the low end hits your amp/bus processing. |
| **HiCut** | 2000 – 20000 | 20000 (off) | Hz | Post-convolution low-pass filter. At its maximum (20 kHz, the default) it's fully bypassed. Lower it to tame fizz, harshness, or excessive top-end from a bright IR — a classic move on high-gain metal guitar tones. |
| **IR Blend** | 0 – 100 | 0 (IR A only) | % | Crossfades between IR A (0%) and IR B (100%). See [IR Blend](#ir-blend). Has no audible effect unless an IR is loaded into slot B. |
| **Distance** | 0 – 100 | 0 (off) | % | Simulated mic-to-cab distance: reduces proximity-effect bass and adds high-frequency darkening as the value increases. See [Distance](#distance-simulated-mic-distance). |
| **Mix** | 0 – 100 | 100 (fully wet) | % | Dry/wet blend of the fully-processed signal against your original input. Lower it for a parallel/blended cab tone, or to taste-test how much of the IR's character you actually want. |
| **Level** | -24 – +24 | 0 | dB | Output trim, applied last. Use it to match gain staging after swapping IRs or dialling in Mix/Blend/Distance, all of which can shift the overall level. |
| **Blend Mode** | Crossfade / Morph | Crossfade | — | How IR A and IR B are combined. See [Blend Mode](#blend-mode-crossfade-or-morph). |
| **IR Align** | Legacy / Precise | Precise | — | How IR B is time-aligned against IR A. Sessions saved before v0.3.0 open as Legacy. See [IR Align](#ir-align). |
| **IR B Trim** | -24 – +24 | 0 | dB | Level of the IR B branch only. |
| **IR B Polarity** | off / on | off | — | Inverts the IR B branch. |
| **IR B Delay** | -5 – +5 | 0 | ms | Timing offset between the two slots. See [IR B Trim, Polarity and Delay](#ir-b-trim-polarity-and-delay). |
| **IR Gain Match** | Energy / Loudness | Energy | — | How loaded IRs are levelled against each other. See [IR Gain Match](#ir-gain-match). |
| **IR A Min-Phase** | off / on | off | — | Minimum-phase transform on slot A. See [Min-Phase](#min-phase-per-slot). |
| **IR B Min-Phase** | off / on | off | — | Minimum-phase transform on slot B. |
| **Distance Air** | off / on | off | — | Adds mic-distance time of flight to the wet path. See [Distance Air](#distance-air). |
| **LoCut Slope** | 12 / 24 dB/oct | 12 | — | LoCut filter steepness. |
| **HiCut Slope** | 12 / 24 dB/oct | 12 | — | HiCut filter steepness. |

Every parameter added in v0.3.0 defaults to a value that changes nothing, so a session saved in an earlier version sounds identical after you upgrade.

## Presets

A preset bar sits at the top of Nave's editor: `[<] [PresetName] [>] [Save] [Save As...] [Delete] [Import...] [Export...]`. Click the preset name to open the full list (factory presets first, then your own, both alphabetical); `<`/`>` step through the same list. Ten factory presets ship with Nave — see [`docs/presets.md`](presets.md) for what each one is for. Your own presets save to `~/Library/Audio/Presets/Yves Vogl/Nave/` on macOS (`%APPDATA%\Yves Vogl\Nave\Presets\` on Windows); "Set current as default" (in the preset menu) controls what a freshly inserted instance of Nave loads. Import/Export both accept single preset files; Import also accepts a `.zip` preset bank exported by `PresetManager::exportBank()`.

## A note on the three "reset" switches

**IR Gain Match**, the two **Min-Phase** toggles, and **IR Align** each change what is actually loaded into the convolution engine, and reloading an engine restarts it — you may hear a brief discontinuity if you change one while audio is playing. This is the same behaviour Nave has always had when you load an IR file, and these are settings you dial in once rather than automate.

Everything continuous is click-free: Blend, Mix, IR B Trim, IR B Polarity, IR B Delay, Distance, and the slope switches are all safe to automate.

## Latency

Nave reports **zero latency in every configuration**, including Morph, Distance Air, IR B Delay and the 24 dB/oct slopes. Those delays are deliberate effects on the wet path, not processing latency — reporting them would make your host shift the entire track to compensate, which is not what you asked for when you moved the mic back.

Nave uses JUCE's zero-latency convolution algorithm by design — cab IRs used for reamping are short, and reamping/tracking workflows are latency-sensitive, so Nave never reports plugin delay compensation to the host. This holds regardless of how many of the above features (IR Blend, Distance, LoCut/HiCut) are engaged.

## Tips

- **Start with LoCut/HiCut at their defaults (off)** and only bring them in if the raw IR needs shaping — a well-captured cab IR often doesn't need much, if any, extra filtering, and adding filters you don't need just costs headroom and CPU for no benefit.
- **For a punchier metal rhythm tone**, try blending a tight, close-mic'd 4x12 IR (IR A) with a small amount of a slightly darker/roomier IR B (10-25% Blend) rather than reaching for a second cab-sim plugin.
- **Distance is a finishing touch, not a tone-shaping tool** — if you need a specific frequency response, use LoCut/HiCut (or your EQ downstream) instead; Distance is meant for a light "push it back in the room" adjustment.
- **If a loaded IR sounds thin or boxy after Blend/Distance changes, check Level** — none of Mix, Blend, or Distance are gain-compensated against each other, by design (so you always know exactly what you're hearing), which means Level is your one-stop place to correct any resulting level mismatch before it hits your mix bus.
- **Null-test your default settings** if you're ever unsure whether Nave is coloring your signal: with no IR loaded (or IR A left at its default) and LoCut/HiCut/Distance all at their defaults, Nave is a certified bit-accurate passthrough (see the project's own null tests in `tests/EngineTests.cpp` and `tests/CoverageTests.cpp`).
