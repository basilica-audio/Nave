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
