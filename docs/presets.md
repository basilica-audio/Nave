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

Three presets reference specific bundled IRs (#42): **Even Blend** and
**Touch of Room Mic** name *4x12 Ceramic Cone* + *4x12 Room 1m* — the
close+room pair their own descriptions call for — and **Mic Morph** names
*4x12 Ceramic Cone* + *4x12 Ceramic Edge*, two positions on the same cabinet.
Before that they were silent no-ops out of the box, because they blend a
slot B nothing had loaded.

The reference is **optional and overridable**. Every other preset carries
none and behaves exactly as before: loading an IR into slot A/B is a separate,
explicit user action (see `docs/manual.md`, "Loading impulse responses"), and
loading a preset and then swapping the cab is the intended thing, not a fight
with the format.

References resolve by **content hash, never by name**. If a referenced IR is
not present the preset still loads with every parameter applied, the currently
loaded IR is left in place, and a notice names what was expected — no
substitution, ever. That matters more than it sounds: a substituted cab would
let the preset keep loading, keep looking correct, and quietly recall a
different sound, which is precisely the failure a preset exists to prevent.
### Where a reference resolves from (#45)

Two sources, consulted in this order:

1. **The user's IR library** — the folder the browser is pointed at, then the
   default `Music/Nave/Impulse Responses`.
2. **Nave's embedded copy** — the bundled cabinets are compiled into the
   binary, so the three factory references above resolve **out of the box**,
   with nothing installed and no button pressed. The bytes are written to
   `<user app data>/Basilica Audio/Nave/Bundled Impulse Responses` and then
   loaded through the same file path a browser selection uses.

The ordering decides which **file** the slot points at, never which **sound**
comes out: a digest can only match bytes equal to it, so when both sources hold
a reference they hold the same audio. The user's own copy wins because it is
the one they can see, move, rename or replace — preferring the invisible one
would make **Install Library** pointless.

**The embedded copy is a resolution source, not a library.** It is never
scanned and never listed; it only ever answers an explicit reference to
specific bytes. **Install Library** remains the single act that puts the
bundled cabinets into the folder the browser lists, and its cache folder is
deliberately not that folder — deleting the cache costs nothing, the next
reference that needs a file re-creates it.

A reference that neither source holds still behaves exactly as described
above: parameters applied, slot untouched, notice raised, nothing substituted.
A **retuned** bundled cabinet is exactly this case — its bytes changed, so its
digest changed, and the preset misses loudly rather than recalling a different
sound under the same name.

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
