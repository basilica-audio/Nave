# 4. The bundled IR library: what ships, and what may change about it

* Status: accepted
* Deciders: Yves Vogl
* Date: 2026-08-27
* Issue: [#33](https://github.com/basilica-audio/Nave/issues/33)

## Context and Problem Statement

Nave is a cabinet IR loader that shipped without cabinets. #33 set the terms for
fixing that: a **licensing bar** ("beyond doubt" — CC0 or public domain, from a
verified source, licence committed alongside the audio), a **labelling bar** (no
synthetic IR may be mistakable for a capture), and a statement that **curation is
a listening decision**.

Two questions remain once audio exists at all:

1. **What is in the library** — how many cabinets, which ones, and at what size?
2. **What may change about it later** — a bundled IR that is renamed, retuned or
   removed in a version bump has consequences for every preset that references
   it (#42, #45).

## Decision Drivers

* The licensing bar is a **gate**, not a preference: an IR whose provenance
  cannot be established does not ship, whatever it sounds like.
* Presets reference IRs by a hash of their **bytes** (#42). Anything that
  changes the bytes of a shipped file breaks every preset made against it.
* The plugin's focus is heavy music (`docs/design-brief.md`), and the two
  headline features — IR Blend and Morph — are only meaningful on **pairs** of
  related captures.
* Binary size is a real cost paid by every user on every install.
* Nobody has listened to these files against the design brief. That is stated
  plainly in #41 and it is still true.

## Considered Options

* **A** — Source a third-party CC0 cabinet-IR pack and bundle it.
* **B** — Ship no bundled content and keep the browser empty.
* **C** — Ship a small, generated, reproducible set and treat its size and
  membership as a release-policy question rather than an open-ended sourcing
  effort.

## Decision Outcome

**Option C, at nine cabinets, held.**

### Licensing: the gate is cleared at the root, not argued past

Nothing in `resources/irs/` is a recording. Every file is computed by
`tools/ir-synth/cabsynth.py` from a documented analytical model, byte-identically
on regeneration (independently verified on a second machine, and cross-checked in
CI against the SHA-256 in `manifest.json`). There is no third-party rights-holder
to trace, no licensor to find and nothing to dispute. All nine are dedicated to
the public domain under **CC0 1.0 Universal**, with the legal code committed at
`resources/irs/CC0-1.0.txt` and per-file provenance at `resources/irs/LICENSES.md`.

Option A was rejected on exactly this ground. A cabinet capture carries rights
from the cabinet, from the microphone and from whoever pressed record; the
popular free packs carry unclear or non-redistributable terms; mirrors routinely
misstate them; and "free to download" is not a licence. Redistributing one inside
a commercial closed-source binary is a claim about somebody else's rights that we
would be unable to substantiate. **Reproducibility is the provenance** — a
stronger position than a licence file we did not author.

Option B was rejected because an IR loader with no IRs is not usable out of the
box, and after #45 the bundled set is what makes the factory presets work at all.

### Membership: nine, and these nine

| id | family | role in the set |
|---|---|---|
| `guitar-412-cone` | guitar | the reference voicing; the "up" cabinet for most heavy tones |
| `guitar-412-edge` | guitar | the **same** cabinet off-axis — the pair Morph needs |
| `guitar-412-room` | guitar | the **same** cabinet at 1 m — the close+room pair IR Blend needs |
| `guitar-212-alnico` | guitar | open-back dipole roll-off; the structural counterpoint to a sealed 4x12 |
| `guitar-112-combo` | guitar | small-box honk; the deliberately un-heavy option |
| `bass-810-cone` | bass | the reference bass voicing |
| `bass-810-edge` | bass | the same cabinet off-axis — the bass Morph pair |
| `bass-115-vintage` | bass | the dark end (ported 1x15, ribbon) |
| `bass-410-horn` | bass | the bright end (horn-loaded, crossover comb) |

The set is built around **pairs**, not around breadth. Three of Nave's ten
factory presets are pair-dependent and were silent no-ops without them (#44);
the two pairs above are what make them audible, and `guitar-412-*` is a genuine
three-way family (cone / edge / room) rather than three unrelated cabinets. The
four remaining slots buy structural variety — sealed vs. ported vs. open-back
dipole, cone vs. horn — which is what actually sounds different, rather than
another 4x12 with different EQ.

**Nine is a stopping point, not a target.** Generated IRs are nearly free in
bytes, which is exactly why the number needs a stated reason to stop: every
additional model is another thing nobody has listened to, another entry in
`LICENSES.md` to keep true, and another file whose bytes a preset may come to
depend on. The set covers both instruments, both mic-position pairs the DSP
features require, and the sealed/ported/open-back/horn axes. Adding a tenth
buys less than the listening pass below does.

### Footprint

**133,840 bytes (130.7 KiB) of embedded assets per architecture slice** — 98,700
bytes of audio (nine mono 24-bit 48 kHz WAVs, 2048–8192 samples) plus 35,140
bytes of provenance (`LICENSES.md`, `CC0-1.0.txt`, `manifest.json`), which travel
with the audio because the licensing bar is a licence committed *alongside* it.
`tests/BundledIrCurationTests.cpp` pins the figure against the bytes actually
compiled in, so it cannot drift silently — including when `LICENSES.md` itself is
edited, since it ships too.

### What may change later: rename freely, never retune in place

Presets resolve by a hash of the file's **bytes**, so the release policy for the
bundled set is:

* A bundled IR may be **renamed** freely. The digest is unaffected, so no preset
  notices. This is what `FactoryIrAsset::stableId` is for on the other side: the
  `id` in `manifest.json` (`guitar-412-cone`) is how documentation, release
  notes and sibling plugins refer to the same cabinet across a rename. The id is
  **never** a resolution key — an identity-based lookup would silently follow a
  retuned model, which is the exact failure #42 chose a byte hash to avoid.
* A bundled IR is **never retuned in place**. Changing a shipped model's voicing
  changes its bytes, and every preset referencing it would miss. A retune ships
  as a **new id and a new file**, with the old file left in the bundle.
* **Removing** a bundled IR is a breaking change for presets and belongs in a
  major version, not a patch.
* An id, once shipped, is **permanent**. `tests/BundledIrCurationTests.cpp`
  carries the nine ids as a literal list; removing or renaming one fails the
  build, which is the ratchet that makes the two rules above enforceable rather
  than merely written down.

Nothing here forbids the library growing. Adding a new id is additive and
affects no existing preset.

### The listening pass

Scoped as a **revision** gate, not a **ship** gate — the set is defensible on
structural grounds, is honest about being modelled, and is what makes the
factory presets audible, so holding a release for it would trade a working
library for an empty one. Tracked as #47 so that #33 does not stay open forever
over a question that is not about mechanism.

**Discharged by measurement wherever measurement is stronger (#47).** Most of
what the pass would decide is not a matter of taste at all, and for those parts
a measurement beats a listening session on every axis that matters here: it is
repeatable, it does not depend on whose monitors it ran through, and it becomes
a regression test that runs on every commit rather than a memory of an
afternoon. `tools/ir-synth/measure_irs.py` characterises the nine as a set and
gates CI on the result; `tests/BundledIrVoicingTests.cpp` pins the same
properties through the plugin's own reader, `IrLoudness` and `IrAlignment`, so a
re-bake cannot quietly change what a cabinet sounds like while still producing
nine files that pass every other gate.

What it found:

* **Polarity is consistent.** All nine present an upright direct arrival, and
  each reads as upright against its family's reference cabinet through
  `IrAlignment::measure()`. This is a correctness property rather than a
  preference: Crossfade blend sums the two convolver branches, so an inverted
  member of a pair would partially cancel the other, and
  `MinPhase::estimateBulkDelaySamples()` searches for a **positive**
  correlation peak, so an inverted IR would hand Morph a meaningless bulk
  delay.
* **The pairs pair.** Both `*-edge` cabinets are darker than their `*-cone`
  partner in the direction an off-axis move implies — 0.55 octaves of −10 dB
  bandwidth and 6.5–7.2 dB of 2–5 kHz — and `guitar-412-room` decays 13.8×
  longer than `guitar-412-cone`. An onset-aligned 50/50 blend of each pair
  sums within 0.30 dB of a perfectly coherent sum, against the 3.0 dB an
  incoherent sum would cost, so none of the three combs.
* **Structural variety is real, not just claimed.** The roll-off order at each
  cabinet's own −3 dB corner separates the three enclosure types with margin:
  open-back dipole 5.1–8.5 dB/octave (first order, as a dipole must be),
  sealed 13.4–14.6 (second order), ported 25.4–28.0 (fourth order).
* **Nothing is truncated audibly.** The Schroeder decay is at least 29 dB down
  where the generator's raised-cosine fade begins, so the fade shapes under
  0.12% of any cabinet's energy.
* **Level match is the one bound the set misses, and no change to these files
  could fix it.** Within a family, K-weighted loudness against a band-limited
  pink source spreads 2.1–2.7 dB in Energy mode and 3.7–4.2 dB in Loudness
  mode, against a 1 dB target derived from the level JND for broadband
  programme. But a bundled IR's own level never reaches anybody: the engine
  renormalises at load in both modes, so rescaling the WAVs would change
  nothing audible while breaking every preset that references them by byte
  hash. The residual belongs to the gain match, which references a **white**
  excitation and is therefore exact for white and drifts for any tilted
  source. That is an engine question, tracked separately; both tests carry the
  measured spread as a ratchet so it cannot get worse in the meantime.
* **Two slots are close to their neighbours.** Level-matched, `guitar-112-combo`
  sits 1.2 dB RMS from `guitar-412-cone` through the mids and highs and differs
  mainly below 250 Hz (4.2 dB RMS), and `bass-115-vintage` sits 0.6–0.7 dB RMS
  from `bass-810-edge` below 1.5 kHz. Both still earn their slots — a genuinely
  different low end is a genuinely different guitar sound, and the two bass
  cabinets diverge above 1.5 kHz and in enclosure order — but neither buys as
  much as its row in the membership table implies. Recorded rather than acted
  on: removing an id is a breaking change for presets.

**What measurement does not close** is whether a cabinet is musically
convincing in a mix. That judgement stays with #47 and is not a property any
script can assert.

### Consequences

* Good: the licensing question is closed at the root and stays checkable rather
  than asserted.
* Good: the bundled set has a stated membership rationale and a stated stability
  policy, so a future change to it is a decision rather than an accident.
* Good: 130.7 KiB is a footprint nobody has to think about.
* Bad: the cabinets are models. They are labelled as such everywhere —
  `modelled_` filenames, "Modelled" display names, both asserted by tests — but a
  user who wants a capture of a specific real cabinet still has to supply it.
* Bad: the set is still unaudited by ear on the one question measurement cannot
  answer — whether a cabinet is musically convincing in a mix. Any of the nine
  may turn out to want revoicing, and under the policy above a revoicing ships
  as a new id rather than as an edit.
* Bad: the set is not level-matched to within the ~1 dB the level JND would
  ask for, and cannot be made so from `resources/irs/` — the fix, if it is
  taken, is to reference the gain match to a tilted source rather than a white
  one, which changes an audible behaviour of a shipped parameter mode.
