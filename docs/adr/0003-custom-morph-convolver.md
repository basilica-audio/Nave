# ADR 0003: A custom convolver for the morph path

- Status: Accepted
- Date: 2026-07-27
- Supersedes: none
- Context: Nave v0.3.0 "First-Class Cab Engine"

## Context

v0.3.0's differentiating feature is mic-position morphing: as the Blend control
moves, Nave interpolates a *new* impulse response from IR A and IR B rather
than crossfading two convolvers. The interpolated IR is recomputed off the
audio thread and republished whenever Blend moves — in practice up to 30 times
a second while the user drags the knob.

That makes IR exchange a continuous, ordinary operation rather than a rare one,
and it is the requirement the rest of this decision follows from.

`juce::dsp::Convolution` cannot meet it. Its `loadImpulseResponse()` swaps the
engine wholesale and resets the internal state, which is audible as a
discontinuity. That is entirely reasonable for its intended use — loading a new
IR file is a deliberate, occasional act — but a morph that clicked 30 times a
second while the user dragged a knob would be unusable.

## Decision

Implement a custom uniformly-partitioned overlap-save convolver,
`src/dsp/MorphConvolver.{h,cpp}`, used **only** on the morph path. The two
stock `juce::dsp::Convolution` instances remain in place and unchanged for the
Crossfade path, which is the default.

The design turns on one structural choice: **one shared frequency-domain delay
line of input spectra, two sets of filter spectra.** On publication the audio
thread runs both filter sets against that single shared input history and
crossfades the two output signals. Because the input history is shared and
never reset, there is no state discontinuity at any point — only two valid
outputs of the same input, blended.

Three consequences follow:

- **The exchange crossfade is amplitude-complementary (linear), not
  equal-power.** This is the one place where we deliberately diverge from the
  research inputs, which prescribed sin/cos gains. Equal-power gains preserve
  *power*, which is correct only for **uncorrelated** material. Successive
  morph spectra are the opposite of uncorrelated — adjacent blend steps are
  nearly identical, and republishing an unchanged IR is perfectly correlated.
  Under sin/cos gains a perfectly-correlated exchange sums to sqrt(2) at the
  fade midpoint: a +3 dB bump on a swap that should be inaudible, and audible
  pumping once a Blend drag makes fades near-continuous. Linear complementary
  gains (`g_next = t`, `g_current = 1 - t`) sum to exactly 1 at every point,
  which makes an identical-IR republish null exactly and a correlated swap
  level-flat.

- **Latency stays zero.** Each callback re-transforms the accumulated input
  segment rather than waiting for a partition to fill, so the first output
  sample is available in the same callback as the first input sample. Nave's
  whole premise is a latency-free reamping chain; a morph that cost a partition
  of delay would have been a worse trade than no morph at all.

- **Publication is lock-free on the audio thread.** The worker fills a pending
  spectra slot and flips an atomic; the audio thread's side of the handshake is
  a non-blocking try, so a publication in progress can never stall a callback.
  Latest-wins, so a fast Blend drag never builds a backlog of stale
  intermediate IRs.

## Alternatives considered

**Keep `juce::dsp::Convolution` and accept the click.** Rejected: the click is
the entire problem. A morph that clicks at every update is not a morph.

**Ping-pong two stock `juce::dsp::Convolution` instances with an output
crossfade.** This was the documented fallback if the custom convolver missed
its correctness or CPU targets, and it would have worked — but it doubles the
convolver count on a path that already runs two, and the swap granularity is
coarser because each instance must fully reload. It remains the escape hatch
if the custom engine ever proves troublesome; the `MorphEngine` interface would
not change.

**Vendor a third-party convolver** (HiFi-LoFi/FFTConvolver is MIT, and so
AGPLv3-compatible). Rejected: the requirement is not "a convolver", it is "a
convolver with a specific, unusual IR-exchange property", which no off-the-shelf
library provides. Vendoring one would still have left the exchange mechanism to
write, plus a dependency to carry.

## Consequences

**We now own a convolver**, which is a real maintenance obligation — this is
core DSP that has to be right at every partition boundary and every block size.
That obligation is discharged by making its correctness tests merge gates:

- nulls against direct time-domain FIR convolution below 1e-5 at IR lengths
  1, 173, 255, 256, 257, 512, 1024 and 16384 — the boundaries either side of a
  partition, a prime length landing mid-partition, and the maximum supported
  length;
- exact zero latency (a delta in produces the IR's first tap at output index 0);
- chunk-size invariance under seeded randomised block sizes, down to the
  single-precision rounding floor;
- an identical-IR republish nulling below -100 dBFS mid-signal, which is the
  assertion the equal-power crossfade would have failed outright.

**The two convolution paths can drift.** Crossfade and Morph are now separate
implementations, and a fix applied to one will not automatically apply to the
other. This is accepted: the paths are deliberately different, and the default
remains the well-tested stock one.

**CPU is bounded and measured.** The full chain with the morph path active and
one forced update per second measures 4.2% of real time at 48 kHz / 128 samples
stereo in a **Debug** build; the release figure is substantially lower. A
`[.benchmark]`-tagged test records this on demand rather than gating CI on a
wall-clock measurement.
