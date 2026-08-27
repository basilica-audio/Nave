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

**132,837 bytes (129.7 KiB) of embedded assets per architecture slice** — 98,700
bytes of audio (nine mono 24-bit 48 kHz WAVs, 2048–8192 samples) plus 34,137
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

**Not discharged, and deliberately not closed by measurement.** #33's own binding
language says curation is a listening decision, and #41 states plainly that
nobody has listened to these. Everything that could be settled without ears has
been: reproducibility, licensing, labelling, signal properties, load-through-the-
engine behaviour, and now membership and footprint.

The listening pass is therefore scoped as a **revision** gate, not a **ship**
gate. The set above is defensible on structural grounds, is honest about being
modelled, and is what makes the factory presets audible; holding the release for
it would trade a working library for an empty one. It is tracked as its own
issue so that #33 does not stay open forever over a question that is not about
mechanism.

### Consequences

* Good: the licensing question is closed at the root and stays checkable rather
  than asserted.
* Good: the bundled set has a stated membership rationale and a stated stability
  policy, so a future change to it is a decision rather than an accident.
* Good: 127.2 KiB is a footprint nobody has to think about.
* Bad: the cabinets are models. They are labelled as such everywhere —
  `modelled_` filenames, "Modelled" display names, both asserted by tests — but a
  user who wants a capture of a specific real cabinet still has to supply it.
* Bad: the set is unaudited by ear. Any of the nine may turn out to want
  revoicing, and under the policy above a revoicing ships as a new id rather than
  as an edit.
