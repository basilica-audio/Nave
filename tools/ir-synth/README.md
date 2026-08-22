# `ir-synth` — cabinet impulse response generator and verifier

Two scripts, both **standard-library-only Python 3**:

| | |
|---|---|
| [`cabsynth.py`](cabsynth.py) | Computes cabinet impulse responses from analytical models and writes them as mono 24-bit WAVs. |
| [`verify_irs.py`](verify_irs.py) | Measures WAVs on disk — format, clipping, DC, normalisation, decay, bandwidth — and fails on any that do not clear the thresholds. Runs in CI. |

## Why this exists

Nave's bar for bundling IR content is "beyond doubt" licensing, and bundling a
cabinet IR means redistributing a recording inside every copy of an AGPLv3
binary. A cabinet capture carries rights from the cabinet, from the
microphone and from whoever pressed record; third-party packs frequently
misstate all three, and "free download" is not a licence. Sourcing one that is
beyond doubt is a research project with an uncertain answer.

An IR you *generate* has no such question. There is no third-party recording in
it, so there is nothing to trace and no licensor to find. Shipping the generator
with the audio is what makes that claim checkable: run it, get byte-identical
output, compare the SHA-256.

**Reproducibility is the licence argument.** That is also why there are no
dependencies. A generator that needs a pinned scientific stack to reproduce its
output is a weaker claim than one that needs nothing but `python3`.

## These produce models, not captures

Nothing this script emits is a recording, a measurement, or a capture of any
real cabinet, speaker or microphone — and none of the models is named after one.
Every output filename begins with `modelled_` and every display name with
"Modelled", and the plugins' test suites assert that rather than leaving it to
review. The labelling is load-bearing: the one way a synthetic IR becomes a
problem is a user assuming it is a capture.

## Usage

```
python3 cabsynth.py --list
python3 cabsynth.py --out-dir ../../resources/irs --models all \
    --manifest ../../resources/irs/manifest.json

python3 verify_irs.py ../../resources/irs \
    --expect-manifest ../../resources/irs/manifest.json --min-files 9
```

`--models` takes model ids, families (`bass`, `guitar`) or `all`.
`--expect-manifest` cross-checks every file's SHA-256 against the committed
provenance record, so documentation cannot drift away from audio.

## How a model is built

A model is one or more **parallel paths** — a woofer path, optionally a horn
path — each with a gain, an integer sample delay, and an ordered chain of biquad
sections. The summed direct field then runs through a sparse FIR of
**reflection taps** whose delays come from a physical excess path length in
centimetres at 343 m/s, and is optionally summed with a decaying **diffuse
tail** from an explicitly-seeded 64-bit LCG.

What the filter sections model, in the order they are usually written:

1. **Driver and box alignment** — a second-order high-pass for a sealed
   cabinet, two cascaded ones for a vented cabinet, a first-order section for an
   open back's dipole roll-off. The `Q` is where a cabinet's punch lives; a flat
   Butterworth alignment sounds like nothing in particular.
2. **Box and baffle** — the low-mid dip that driver spacing and baffle geometry
   produce.
3. **Cone breakup modes** — the peaks and the null between them. These are what
   make a 12" guitar speaker sound like one at all.
4. **Voice-coil inductance and cone mass** — the steep high-frequency cliff,
   usually 24 dB/oct. This is the part no EQ curve quite reproduces.
5. **Microphone** — proximity rise (a low-shelf; strong on a ribbon, moderate on
   a close cardioid dynamic), presence peak, and for an off-axis or cone-edge
   position a high-shelf cut plus an extra low-pass pole for the cone's
   directivity.

Then, after rendering: truncation to the target length with a raised-cosine
fade, and normalisation.

### Length is a low-frequency decision

The high-pass alignment rings with a time constant of roughly `Q / (pi * f0)`. A
55 Hz sealed bass alignment at Q 1.15 has a 6.6 ms time constant, so a
1024-tap (21 ms) IR cuts it after about three of them and leaves a DC term
around −26 dB relative to the response peak — which is audible as an IR that
walks the signal off centre. The bass models are therefore 4096 taps, the guitar
models 2048, and `verify_irs.py` fails anything whose DC term is not at least
60 dB down.

### Normalisation is `max |H(f)| = 1.0`

Not peak-sample, not energy. It is the criterion that actually bounds the
convolution: with the maximum of the magnitude response at unity, no sine at any
frequency can leave the convolver louder than it entered, so the IR alone cannot
drive a full-scale input into clipping. Perceptual level-matching between IRs is
a plugin concern, not a file concern.

## What `verify_irs.py` checks

Read back off disk, i.e. the exact bytes that ship:

- mono, 24-bit PCM, at the expected sample rate;
- no sample on the full-scale code, no non-finite sample;
- DC offset below 1e-4, and `|H(0)|` at least 60 dB below the response peak;
- `max |H(f)|` within 0.05 dB of 0 dBFS;
- a measurable T20-based decay time inside a sane range;
- the last samples at or below −90 dBFS, i.e. the fade actually closed rather
  than the IR being cut off mid-ring;
- `−3 dB` and `−10 dB` corners and a per-octave response table, read off a
  1/3-octave RMS-smoothed curve. Smoothing first matters: a cabinet response is
  a comb of narrow breakup peaks, and walking outwards from a raw-spectrum
  maximum measures the skirt of one peak rather than the cabinet's bandwidth.

Exit status is non-zero on any failure, so it gates CI rather than producing a
report nobody reads.

## Adding a model

Add an entry to `MODELS` in `cabsynth.py` via `_register(...)`, re-run the
generator and the verifier, and add a description to `resources/irs/LICENSES.md`.
Keep the `modelled_` filename prefix — if an asset is ever *not* generated, it
must not have one.
