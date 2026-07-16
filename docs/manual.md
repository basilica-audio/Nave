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

Both slots' file paths are saved with your session/preset, so a project reopens with the same cabs loaded.

### IR Blend

Two different loaded IRs can end up sounding noticeably different in level even at identical Distance/LoCut/HiCut/Mix settings — Nave normalises each loaded IR's *energy* to a consistent reference (not its perceived loudness), and real-world cab IRs vary enough in length/spectral content that the same energy target can still land at different subjective volumes. This isn't a bug to work around with EQ — reach for **Level** to match gain staging after swapping IRs.

The **IR Blend** knob crossfades between IR A (0%) and IR B (100%). Typical uses:

- **Two different cabs** — blend a tight 4x12 with a boomier 2x12 to taste, without needing a separate blending plugin.
- **Two mic positions on the same cab** — e.g. an on-axis close mic (IR A) blended with a room/ambient mic (IR B) for more dimension.

When you load IR B, Nave automatically **phase-aligns** it to IR A's transient onset before the two are ever mixed together. Two real-world IR captures rarely start at exactly the same moment (different mic distances, different capture setups), and blending misaligned IRs directly would partially cancel a wide band of frequencies (comb filtering) — the alignment step prevents that, so IR Blend sounds like a genuine tonal blend rather than a phasey mess.

Blend defaults to 0% (IR A only) — loading an IR B and leaving Blend at 0% has no audible effect until you turn the knob up.

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

## Presets

A preset bar sits at the top of Nave's editor: `[<] [PresetName] [>] [Save] [Save As...] [Delete] [Import...] [Export...]`. Click the preset name to open the full list (factory presets first, then your own, both alphabetical); `<`/`>` step through the same list. Eight factory presets ship with Nave — see [`docs/presets.md`](presets.md) for what each one is for. Your own presets save to `~/Library/Audio/Presets/Yves Vogl/Nave/` on macOS (`%APPDATA%\Yves Vogl\Nave\Presets\` on Windows); "Set current as default" (in the preset menu) controls what a freshly inserted instance of Nave loads. Import/Export both accept single preset files; Import also accepts a `.zip` preset bank exported by `PresetManager::exportBank()`.

## Latency

Nave uses JUCE's zero-latency convolution algorithm by design — cab IRs used for reamping are short, and reamping/tracking workflows are latency-sensitive, so Nave never reports plugin delay compensation to the host. This holds regardless of how many of the above features (IR Blend, Distance, LoCut/HiCut) are engaged.

## Tips

- **Start with LoCut/HiCut at their defaults (off)** and only bring them in if the raw IR needs shaping — a well-captured cab IR often doesn't need much, if any, extra filtering, and adding filters you don't need just costs headroom and CPU for no benefit.
- **For a punchier metal rhythm tone**, try blending a tight, close-mic'd 4x12 IR (IR A) with a small amount of a slightly darker/roomier IR B (10-25% Blend) rather than reaching for a second cab-sim plugin.
- **Distance is a finishing touch, not a tone-shaping tool** — if you need a specific frequency response, use LoCut/HiCut (or your EQ downstream) instead; Distance is meant for a light "push it back in the room" adjustment.
- **If a loaded IR sounds thin or boxy after Blend/Distance changes, check Level** — none of Mix, Blend, or Distance are gain-compensated against each other, by design (so you always know exactly what you're hearing), which means Level is your one-stop place to correct any resulting level mismatch before it hits your mix bus.
- **Null-test your default settings** if you're ever unsure whether Nave is coloring your signal: with no IR loaded (or IR A left at its default) and LoCut/HiCut/Distance all at their defaults, Nave is a certified bit-accurate passthrough (see the project's own null tests in `tests/EngineTests.cpp` and `tests/CoverageTests.cpp`).
