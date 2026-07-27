# Factory presets

Eight factory presets ship with Nave v0.2.0, embedded via BinaryData from
`presets/factory/*.json` (see `docs/preset-system-notes.md` for the build
wiring). All are sourced starting points from `docs/design-brief.md`'s
"Factory Presets" section - see that document's own Honesty section for what
these numbers are and aren't calibrated against (research/forum/manual-
derived, not measured hardware).

| Preset | Category | Intent |
|---|---|---|
| **Default** | Init | The certified passthrough state (all parameters at their off/default position), exposed as an explicit preset so there's always a one-click way back to "no coloration." Also this plugin's out-of-the-box default (see the M2 default-resolution order in `docs/preset-system-notes.md`). |
| **Tame the Fizz** | Guitar | General-purpose high-gain cleanup (LoCut ~100 Hz / HiCut ~5 kHz), sourced from Fractal Audio community consensus. |
| **Live Stage** | Guitar | Tighter, more aggressive cut (LoCut ~80 Hz / HiCut ~8 kHz) for a live monitoring/tracking chain where mud and fizz both cost headroom. |
| **Dark Vintage** | Guitar | Darker, narrower-band vintage/lo-fi cab character (LoCut ~180 Hz / HiCut ~4.5 kHz) plus a light Distance push (~25%) for extra proximity darkening. |
| **Pushed Back in the Room** | Guitar | Showcases Distance alone (~60%) as the sourced "finishing touch" it's documented to be, with a small Level compensation for the resulting shelving cuts. |
| **Touch of Room Mic** | Guitar | Showcases IR Blend at the sourced low-ratio end (15%) for "a touch of a second mic" - requires an IR loaded into slot B to be audible. |
| **Even Blend** | Guitar | The sourced 50/50 discrete stopping point for two genuinely complementary IRs (two cabs, or close+room) - requires an IR loaded into slot B to be audible. |
| **Parallel Cab (Blended Dry)** | Guitar | Showcases Mix as a genuine parallel-processing tool: a moderate Distance push (~20%) blended with a partial Mix (~65%) for a thickened, less "all-or-nothing" cab tone. |

None of the presets reference specific IR files - loading an IR into slot A/B
is always a separate, explicit user action (see `docs/manual.md`'s
"Loading impulse responses" section). "Touch of Room Mic" and "Even Blend"
only become audible once something real is loaded into slot B.

## v0.3.0 additions

Both new presets need **two IRs loaded** to do anything — like every blend-based preset here, they set the controls, not the cabinets.

### Mic Morph

`blendMode = Morph`, `irBlend = 35%`, `irGainMode = Loudness`.

The release's headline feature as a starting point. Load two captures of the *same* cabinet at different mic positions — on-axis and off-axis, or cap-edge and cone-centre — and sweep IR Blend. Instead of crossfading between them (which combs wherever their arrivals differ), Morph interpolates a single new impulse response, so every intermediate position sounds like a real mic placement rather than two mics fighting.

Loudness gain matching is on so that swapping either capture does not also change the level.

Start at 35% and drag Blend while the track plays; the useful position is usually wherever the low-mids stop sounding hollow.

### Tight Stack

`irBlend = 45%`, `irBTrim = -2.5 dB`, `irBDelay = +0.35 ms`, `loCut = 75 Hz` at 24 dB/oct, `hiCut = 9 kHz`, `irGainMode = Loudness`.

The dual-mic recipe, in Crossfade mode: a main cabinet in slot A and a second, slightly-trimmed capture in slot B pushed 0.35 ms later. That small offset is deliberate — it is the classic console move for thickening a stacked guitar without the phase cancellation of a hard sum, and it is exactly what Nave's alignment removes by default, so this preset puts a controlled amount of it back.

The 24 dB/oct LoCut at 75 Hz clears the sub-bass more decisively than a 12 dB/oct slope would at the same frequency, leaving the body just above it intact.
