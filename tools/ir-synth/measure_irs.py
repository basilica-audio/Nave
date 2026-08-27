#!/usr/bin/env python3
"""Measure the bundled impulse responses as a SET, not one file at a time.

WHY THIS EXISTS
---------------
`verify_irs.py` answers "is each of these files a sound signal" - format,
clipping, DC, normalisation, decay. That is a per-file gate and it says nothing
about the nine as a library.

Issue #33 declared curation "a listening decision" and #47 carried that forward
as a listening pass. Most of what a listening pass would decide is measurable,
and measurement is strictly stronger wherever it applies: it is repeatable, it
survives a change of monitoring, and it turns into a regression test. What this
script measures is exactly the part of that pass which does not need ears:

  * VOICING       where each cabinet's band actually starts and stops, what its
                  upper-mid character region is, and how fast it decays
  * LEVEL MATCH   whether auditioning one cabinet after another produces a
                  loudness jump that reads as "better" - evaluated through the
                  SAME normalisation the plugin applies at load, because that
                  is the level a user hears (see LEVEL MATCH below)
  * POLARITY      whether any cabinet is inverted relative to the others, which
                  is a defect for IR Blend and Morph rather than a taste
  * PAIRS         whether cone/edge/room differ in the direction their names
                  claim, and whether an aligned 50/50 blend of a pair sums
                  coherently instead of combing
  * VARIETY       whether sealed / ported / open-back are distinguishable on
                  the measurements rather than only in the manifest
  * REDUNDANCY    how close any two cabinets are once level-matched, so a slot
                  that buys nothing is visible as a number

What is NOT in here is anything that genuinely needs ears: whether a cabinet is
musically convincing in a mix. That judgement is recorded on #47.

LEVEL MATCH: MEASURED WHERE THE USER HEARS IT
---------------------------------------------
A bundled IR's own file level never reaches anybody's ears. The engine
renormalises every IR at load - `juce::dsp::Convolution::Normalise::yes` in
Energy mode (JUCE 8.0.14 scales by 0.125 / sqrt(sum of squares)), or
`IrLoudness::applyLoudnessNormalisation` in Loudness mode (0.125 / sqrt of the
K-weighted sum of squares). Measuring the loudness of the shipped bytes would
therefore measure something no user can hear, and "fixing" it by rescaling the
WAVs would change nothing audible while breaking every preset that references
them by byte hash (ADR-0004).

So each cabinet's loudness is reported three times - as shipped, after Energy
normalisation, and after Loudness normalisation - and the verdict is read off
the two normalised columns.

Loudness is computed in the frequency domain rather than by convolving a noise
file, so there is no test signal, no seed and no run-to-run variance:

    L = 10 log10( SUM_k S_k |H_k|^2 |K_k|^2 / SUM_k S_k |K_k|^2 )

with |H| the IR's magnitude response, |K| the ITU-R BS.1770-4 K-weighting
(re-derived here from the same analog prototypes `src/dsp/IrLoudness.cpp` uses,
so the two agree), and S the source's power spectrum. S is swept over several
spectral tilts because the answer depends on it, and a conclusion that only
holds for one source is not a conclusion.

DEPENDENCIES
------------
The Python standard library, and nothing else - the same rule cabsynth.py
follows, and for the same reason.

USAGE
-----
    python3 measure_irs.py ../../resources/irs
    python3 measure_irs.py ../../resources/irs --check
    python3 measure_irs.py ../../resources/irs --json measurements.json
"""

from __future__ import annotations

import argparse
import cmath
import itertools
import json
import math
import os
import struct
import sys

# ---------------------------------------------------------------------------
# Analysis constants
# ---------------------------------------------------------------------------

FFT_SIZE = 65536

# Fraction-of-an-octave smoothing applied before any band edge is read off.
# A raw magnitude response of a cabinet IR is full of reflection comb notches;
# reading a "-10 dB corner" off the unsmoothed curve finds the first notch
# rather than the edge of the band, which is how resources/irs/LICENSES.md came
# to publish a 562 Hz low corner for an IR that is only 1.5 dB down at 125 Hz.
# A third of an octave is the standard resolution for reporting loudspeaker
# response and is narrow enough to keep real resonances.
SMOOTHING_FRACTION = 3

# Band edges are read relative to the mean level over this band rather than to
# the single highest point of the response. A guitar cabinet peaks in the upper
# mids by design, so a peak-referenced "-3 dB corner" lands in the mids and
# describes the peak's width instead of the cabinet's bandwidth. Every one of
# the nine is designed to work inside 100 Hz - 4 kHz; below and above it the
# families deliberately diverge, which is what the edges are there to report.
REFERENCE_BAND_HZ = (100.0, 4000.0)

# Octave centres for the published response table.
OCTAVE_CENTRES = [31.5, 63.0, 125.0, 250.0, 500.0, 1000.0, 2000.0, 4000.0, 8000.0, 16000.0]

# The character bands. "Presence" is where a cabinet's cone-breakup peaks live
# and is what a listener calls its voice; "air" is above the cone's cliff and is
# mostly what separates a horn from a cone.
LOW_MID_BAND_HZ = (200.0, 800.0)
PRESENCE_SEARCH_HZ = (800.0, 6000.0)
PRESENCE_BAND_HZ = (2000.0, 5000.0)
AIR_BAND_HZ = (5000.0, 10000.0)
# The proximity index: how much low-mid lift a microphone placement adds. A
# close dynamic has several dB of it; a metre away there is none in the direct
# field.
PROXIMITY_BAND_HZ = (80.0, 160.0)
PROXIMITY_REFERENCE_HZ = (400.0, 1000.0)

# The engine's two IR gain-match modes, as gains applied to the shipped bytes.
#   Energy   - juce::dsp::Convolution::Normalise::yes, JUCE 8.0.14
#              calculateNormalisationFactor(): 0.125f / sqrt(sumSquaredMagnitude)
#   Loudness - IrLoudness::computeLoudnessGain(): referenceAmplitude /
#              sqrt(K-weighted sum of squares), referenceAmplitude = 0.125
NORMALISATION_REFERENCE_AMPLITUDE = 0.125

# ---------------------------------------------------------------------------
# Thresholds. Every one carries its derivation; none is a round number chosen
# because it looked tidy.
# ---------------------------------------------------------------------------

# POLARITY. Not a tolerance - a sign. An inverted member of a pair partially
# cancels the other under Crossfade blend, which sums the two convolver outputs
# (CabConvolutionEngine::process, aGain / branchGain), and it also breaks Morph:
# MinPhase::estimateBulkDelaySamples looks for a POSITIVE cross-correlation peak
# against the IR's own minimum-phase equivalent, so an inverted IR correlates
# negatively everywhere near its true lag and the search returns a meaningless
# bulk delay.
REQUIRED_POLARITY = +1

# LEVEL MATCH, target. The level JND for broadband programme material is about
# 1 dB (Zwicker & Fastl, Psychoacoustics, ch. 8); ITU-R BS.1116-3 requires level
# matching before any critical A/B precisely because the louder of two otherwise
# equal things is reliably preferred. A family whose members sit inside a 1 dB
# spread cannot be told apart on level alone, which is the property that matters
# when a user is auditioning cabinets against each other in the browser.
TARGET_FAMILY_LEVEL_SPREAD_DB = 1.0

# LEVEL MATCH, ratchet. The set does not meet the target above: the engine's
# gain match references a WHITE excitation, so it is exact for white and drifts
# for any tilted source (see the table this script prints). That is a property
# of the gain match, not of the nine files - rescaling the WAVs cannot fix it,
# because the engine discards their level at load. It is tracked as its own
# issue. What is gated here is that the spread does not get WORSE. The measured
# worst case is 4.20 dB (the bass family in Loudness mode against band-limited
# pink); the ratchet sits at the next half-decibel above it, which is loose
# enough that float drift across platforms cannot redden CI and tight enough
# that a regression of a third of a decibel does.
MAX_FAMILY_LEVEL_SPREAD_DB = 4.5

# BLEND COHERENCE. Two IRs summed 50/50 after onset alignment should behave as
# correlated signals, not as independent noise. Perfectly coherent summing and
# perfectly incoherent summing differ by 3.0 dB, so the whole scale of the
# defect is 3 dB; a loss of a sixth of that is both below the ~1 dB level JND
# and an order of magnitude away from an audible comb.
MAX_BLEND_COHERENCE_LOSS_DB = 0.5

# TRUNCATION. cabsynth.py closes every IR with a raised-cosine fade over its
# last quarter so truncation cannot leave a step. The fade is only honest if it
# is shaping a decay that has already fallen away: at -20 dB on the Schroeder
# curve the fade window holds under 1% of the IR's energy, which cannot change
# the decay a listener hears (the JND for reverberation time is around 5%).
MAX_ENERGY_DECAY_AT_FADE_DB = -20.0

# PAIR CLAIMS. A microphone moved from the dust cap to the cone edge and angled
# off-axis loses high end - that is cone directivity, and it is the entire claim
# both "edge" files make ("the darker half of a cone/edge blend"). Two
# independent readings of the same claim are gated because either alone is
# brittle: the -10 dB edge is one number off a smoothed curve (both pairs
# measure 0.55 octaves, so a third of an octave leaves 60% of margin), while the
# 2 kHz - 5 kHz tilt integrates over the whole presence region (both pairs
# measure 6.5 - 7.2 dB, so 3 dB leaves better than a factor of two).
MIN_EDGE_DARKER_OCTAVES = 0.35
MIN_EDGE_PRESENCE_DROP_DB = 3.0

# The room capture's claim is a room: a decay that is not the close mic's. A
# factor of four is far outside what a reflection pattern alone produces and far
# inside the measured factor of thirteen.
MIN_ROOM_DECAY_RATIO = 4.0

# STRUCTURAL VARIETY. The low-frequency roll-off slope at each cabinet's own
# -3 dB corner is the discriminator, because it is set by the enclosure rather
# than by voicing EQ:
#   open-back dipole  front and rear waves cancel: first order, 6 dB/octave
#   sealed            one second-order box alignment: 12 dB/octave
#   ported            a vented alignment is fourth order below tuning: 24
# The class boundaries below sit in the gaps the measurements leave (the widest
# dipole is 8.5, the narrowest sealed 13.4, the widest sealed 14.6, the
# narrowest ported 25.4), so each has at least 4 dB/octave of margin.
DIPOLE_MAX_SLOPE_DB_PER_OCT = 10.0
SEALED_SLOPE_RANGE_DB_PER_OCT = (11.0, 18.0)
PORTED_MIN_SLOPE_DB_PER_OCT = 21.0

# The enclosure each id models, from cabsynth.py's model definitions. This is
# the claim; the slope above is the measurement that has to agree with it.
ENCLOSURE = {
    "guitar-412-cone": "sealed",
    "guitar-412-edge": "sealed",
    "guitar-412-room": "sealed",
    "guitar-212-alnico": "open-back",
    "guitar-112-combo": "open-back",
    "bass-810-cone": "sealed",
    "bass-810-edge": "sealed",
    "bass-115-vintage": "ported",
    "bass-410-horn": "ported",
}

# The pairs three factory presets depend on (#44), and what each claims.
PAIRS = [
    ("guitar-412-cone", "guitar-412-edge", "off-axis"),
    ("bass-810-cone", "bass-810-edge", "off-axis"),
    ("guitar-412-cone", "guitar-412-room", "room"),
]

# Source spectra to evaluate loudness against, as dB per octave of tilt.
# White and pink bracket any real programme; -6 dB/octave is about as steep as a
# bass DI through an amplifier gets. The two band-limited pink sources check
# that the conclusion does not hinge on energy outside the instrument's range.
SOURCES = [
    ("white 0 dB/oct", 0.0, 20.0, 20000.0),
    ("pink -3 dB/oct", -3.0, 20.0, 20000.0),
    ("prog -6 dB/oct", -6.0, 20.0, 20000.0),
    ("pink, guitar band", -3.0, 70.0, 6000.0),
    ("pink, bass band", -3.0, 30.0, 5000.0),
]

# The source the single-number verdicts are read from. A high-gain guitar or
# bass preamp feeding a cabinet is roughly pink over the band the cabinet
# reproduces, and band-limiting it keeps sub-30 Hz and above-6 kHz energy - none
# of which survives any of these cabinets - from dominating the integral.
VERDICT_SOURCES = {"guitar": "pink, guitar band", "bass": "pink, bass band"}


# ---------------------------------------------------------------------------
# WAV reading (24-bit PCM, the format cabsynth.py writes)
# ---------------------------------------------------------------------------

def read_wav(path):
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("%s: not a RIFF/WAVE file" % path)

    pos, fmt, payload = 12, None, None
    while pos + 8 <= len(data):
        chunk_id = data[pos:pos + 4]
        chunk_size = struct.unpack("<I", data[pos + 4:pos + 8])[0]
        body = data[pos + 8:pos + 8 + chunk_size]
        if chunk_id == b"fmt ":
            fmt = struct.unpack("<HHIIHH", body[:16])
        elif chunk_id == b"data":
            payload = body
        pos += 8 + chunk_size + (chunk_size % 2)

    if fmt is None or payload is None:
        raise ValueError("%s: missing fmt or data chunk" % path)

    audio_format, channels, sample_rate, _, _, bits = fmt
    if audio_format != 1 or bits != 24:
        raise ValueError("%s: expected 24-bit PCM" % path)

    full_scale = float(1 << 23)
    samples = []
    for offset in range(0, len(payload) - 2, 3):
        raw = payload[offset] | (payload[offset + 1] << 8) | (payload[offset + 2] << 16)
        if raw & 0x800000:
            raw -= 0x1000000
        samples.append(raw / full_scale)

    return {"channels": channels, "sample_rate": sample_rate, "samples": samples}


# ---------------------------------------------------------------------------
# FFT (iterative radix-2 Cooley-Tukey, the same one cabsynth.py uses)
# ---------------------------------------------------------------------------

def fft(values):
    n = len(values)
    if n & (n - 1) != 0:
        raise ValueError("FFT length must be a power of two")

    data = list(values)
    j = 0
    for i in range(1, n):
        bit = n >> 1
        while j & bit:
            j ^= bit
            bit >>= 1
        j |= bit
        if i < j:
            data[i], data[j] = data[j], data[i]

    length = 2
    while length <= n:
        angle = -2.0 * math.pi / length
        step = complex(math.cos(angle), math.sin(angle))
        for start in range(0, n, length):
            w = complex(1.0, 0.0)
            half = length >> 1
            for k in range(start, start + half):
                u = data[k]
                v = data[k + half] * w
                data[k] = u + v
                data[k + half] = u - v
                w *= step
        length <<= 1
    return data


def power_spectrum(impulse, fft_size=FFT_SIZE):
    padded = [complex(x, 0.0) for x in impulse] + [0j] * (fft_size - len(impulse))
    spectrum = fft(padded)
    return [abs(spectrum[k]) ** 2 for k in range(fft_size // 2 + 1)]


# ---------------------------------------------------------------------------
# K-weighting - mirrors src/dsp/IrLoudness.cpp so both agree by construction
# ---------------------------------------------------------------------------

def k_weighting_stage1(sample_rate):
    frequency, gain_db, q = 1681.974450955533, 3.999843853973347, 0.7071752369554196
    amplitude = 10.0 ** (gain_db / 40.0)
    omega = 2.0 * math.pi * frequency / sample_rate
    cos_omega, sin_omega = math.cos(omega), math.sin(omega)
    alpha = sin_omega / (2.0 * q)
    shaped = 2.0 * math.sqrt(amplitude) * alpha
    a0 = (amplitude + 1.0) - (amplitude - 1.0) * cos_omega + shaped
    return (amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cos_omega + shaped) / a0,
            -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cos_omega) / a0,
            amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cos_omega - shaped) / a0,
            2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cos_omega) / a0,
            ((amplitude + 1.0) - (amplitude - 1.0) * cos_omega - shaped) / a0)


def k_weighting_stage2(sample_rate):
    frequency, q = 38.13547087602444, 0.5003270373238773
    omega = 2.0 * math.pi * frequency / sample_rate
    cos_omega, sin_omega = math.cos(omega), math.sin(omega)
    alpha = sin_omega / (2.0 * q)
    a0 = 1.0 + alpha
    return (((1.0 + cos_omega) * 0.5) / a0,
            (-(1.0 + cos_omega)) / a0,
            ((1.0 + cos_omega) * 0.5) / a0,
            (-2.0 * cos_omega) / a0,
            (1.0 - alpha) / a0)


def biquad_power_response(coefficients, num_bins, fft_size=FFT_SIZE):
    b0, b1, b2, a1, a2 = coefficients
    response = []
    for k in range(num_bins):
        omega = 2.0 * math.pi * k / fft_size
        z1 = cmath.exp(-1j * omega)
        z2 = z1 * z1
        response.append(abs((b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2)) ** 2)
    return response


def biquad_filter(signal, coefficients):
    b0, b1, b2, a1, a2 = coefficients
    x1 = x2 = y1 = y2 = 0.0
    out = []
    for x0 in signal:
        y0 = b0 * x0 + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1 = x1, x0
        y2, y1 = y1, y0
        out.append(y0)
    return out


def k_weighted_energy(signal, sample_rate):
    filtered = biquad_filter(biquad_filter(signal, k_weighting_stage1(sample_rate)),
                             k_weighting_stage2(sample_rate))
    return sum(v * v for v in filtered)


def energy(signal):
    return sum(v * v for v in signal)


# ---------------------------------------------------------------------------
# Small numeric helpers
# ---------------------------------------------------------------------------

def to_db(power, floor=1e-30):
    return 10.0 * math.log10(max(power, floor))


def amplitude_db(value, floor=1e-15):
    return 20.0 * math.log10(max(abs(value), floor))


def smooth_fractional_octave(power, fraction=SMOOTHING_FRACTION):
    """Power-average every bin over a 1/`fraction`-octave band centred on it."""
    n = len(power)
    cumulative = [0.0] * (n + 1)
    for i in range(n):
        cumulative[i + 1] = cumulative[i] + power[i]

    half_width = 2.0 ** (1.0 / (2.0 * fraction))
    smoothed = [power[0]] + [0.0] * (n - 1)
    for k in range(1, n):
        low = max(1, int(math.floor(k / half_width)))
        high = min(n - 1, int(math.ceil(k * half_width)))
        smoothed[k] = (cumulative[high + 1] - cumulative[low]) / (high - low + 1)
    return smoothed


class Analysis:
    """Frequency-axis bookkeeping shared by every measurement below."""

    def __init__(self, sample_rate, fft_size=FFT_SIZE):
        self.sample_rate = sample_rate
        self.fft_size = fft_size
        self.num_bins = fft_size // 2 + 1
        self.frequencies = [k * sample_rate / fft_size for k in range(self.num_bins)]
        stage1 = biquad_power_response(k_weighting_stage1(sample_rate), self.num_bins, fft_size)
        stage2 = biquad_power_response(k_weighting_stage2(sample_rate), self.num_bins, fft_size)
        self.k_power = [a * b for a, b in zip(stage1, stage2)]
        self.sources = {}
        for name, tilt_db_per_octave, low_hz, high_hz in SOURCES:
            spectrum = [0.0] * self.num_bins
            for k in range(1, self.num_bins):
                frequency = self.frequencies[k]
                if low_hz <= frequency <= high_hz:
                    tilt = tilt_db_per_octave * math.log2(frequency / 1000.0)
                    spectrum[k] = 10.0 ** (tilt / 10.0)
            weighted = [spectrum[k] * self.k_power[k] for k in range(self.num_bins)]
            self.sources[name] = (weighted, sum(weighted))

    def band_mean_db(self, smoothed, low_hz, high_hz):
        total, count = 0.0, 0
        for k in range(1, self.num_bins):
            if low_hz <= self.frequencies[k] <= high_hz:
                total += smoothed[k]
                count += 1
        return to_db(total / max(count, 1))

    def level_at_db(self, smoothed, frequency_hz):
        k = min(range(1, self.num_bins), key=lambda i: abs(self.frequencies[i] - frequency_hz))
        return to_db(smoothed[k])

    def slope_db_per_octave(self, smoothed, frequency_hz):
        """Slope of the smoothed curve over the octave below `frequency_hz`."""
        return self.level_at_db(smoothed, frequency_hz) - self.level_at_db(smoothed, frequency_hz / 2.0)

    def band_edges(self, smoothed, reference_db, drop_db):
        """Outermost frequencies at which the curve is within `drop_db` of the reference."""
        low = high = None
        for k in range(1, self.num_bins):
            frequency = self.frequencies[k]
            if not (15.0 <= frequency <= 22000.0):
                continue
            if to_db(smoothed[k]) >= reference_db - drop_db:
                if low is None:
                    low = frequency
                high = frequency
        return low, high

    def loudness_db(self, power, source_name):
        weighted, total = self.sources[source_name]
        return to_db(sum(power[k] * weighted[k] for k in range(self.num_bins)) / total)


def schroeder_decay_db(impulse):
    total = sum(v * v for v in impulse)
    if total <= 0.0:
        return [0.0] * len(impulse), 0.0
    curve, running = [0.0] * len(impulse), 0.0
    for i in range(len(impulse) - 1, -1, -1):
        running += impulse[i] * impulse[i]
        curve[i] = to_db(running / total)
    return curve, total


def reverberation_time_ms(decay_db, sample_rate, start_db=-5.0, end_db=-25.0):
    start = end = None
    for index, value in enumerate(decay_db):
        if start is None and value <= start_db:
            start = index
        if end is None and value <= end_db:
            end = index
    if start is None or end is None or end <= start:
        return None
    return (end - start) / sample_rate * 1000.0 * (60.0 / (start_db - end_db))


def cross_correlation_peak(a, b, max_lag=512):
    """Lag of the largest-magnitude correlation of `b` against `a`, and its value.

    The SIGN of that value is the polarity of b relative to a - the same test
    IrAlignment::analyse() applies before slot B is blended into slot A.
    """
    best_value, best_lag = 0.0, 0
    length = min(len(a), len(b))
    for lag in range(0, max_lag + 1):
        total = 0.0
        for i in range(length - lag):
            total += a[i] * b[i + lag]
        if abs(total) > abs(best_value):
            best_value, best_lag = total, lag
    return best_lag, best_value


# ---------------------------------------------------------------------------
# Per-IR measurement
# ---------------------------------------------------------------------------

def measure(entry, directory, analysis):
    wave = read_wav(os.path.join(directory, entry["file"]))
    impulse = wave["samples"]
    sample_rate = wave["sample_rate"]
    num_samples = len(impulse)

    power = power_spectrum(impulse)
    smoothed = smooth_fractional_octave(power)
    reference_db = analysis.band_mean_db(smoothed, *REFERENCE_BAND_HZ)

    low3, high3 = analysis.band_edges(smoothed, reference_db, 3.0)
    low10, high10 = analysis.band_edges(smoothed, reference_db, 10.0)

    presence_bin = max((k for k in range(1, analysis.num_bins)
                        if PRESENCE_SEARCH_HZ[0] <= analysis.frequencies[k] <= PRESENCE_SEARCH_HZ[1]),
                       key=lambda k: smoothed[k])
    low_mid_db = analysis.band_mean_db(smoothed, *LOW_MID_BAND_HZ)

    decay_db, total_energy = schroeder_decay_db(impulse)
    fade_samples = max(16, num_samples // 4)
    fade_start = num_samples - fade_samples

    raw_energy = energy(impulse)
    weighted_energy = k_weighted_energy(impulse, sample_rate)
    energy_gain = NORMALISATION_REFERENCE_AMPLITUDE / math.sqrt(raw_energy)
    loudness_gain = NORMALISATION_REFERENCE_AMPLITUDE / math.sqrt(weighted_energy)

    onset_window = max(1, int(0.001 * sample_rate))
    onset_index = max(range(onset_window), key=lambda i: abs(impulse[i]))

    shipped = {name: analysis.loudness_db(power, name) for name, _, _, _ in SOURCES}

    return {
        "id": entry["id"],
        "file": entry["file"],
        "family": entry["family"],
        "display_name": entry["display_name"],
        "samples": num_samples,
        "length_ms": num_samples / sample_rate * 1000.0,
        "reference_db": reference_db,
        "edge_lo_3db_hz": low3,
        "edge_hi_3db_hz": high3,
        "edge_lo_10db_hz": low10,
        "edge_hi_10db_hz": high10,
        "lf_slope_db_per_oct": analysis.slope_db_per_octave(smoothed, low3),
        "enclosure": ENCLOSURE[entry["id"]],
        "presence_hz": analysis.frequencies[presence_bin],
        "presence_lift_db": to_db(smoothed[presence_bin]) - low_mid_db,
        "presence_tilt_db": analysis.band_mean_db(smoothed, *PRESENCE_BAND_HZ) - low_mid_db,
        "air_tilt_db": analysis.band_mean_db(smoothed, *AIR_BAND_HZ) - low_mid_db,
        "proximity_index_db": (analysis.band_mean_db(smoothed, *PROXIMITY_BAND_HZ)
                               - analysis.band_mean_db(smoothed, *PROXIMITY_REFERENCE_HZ)),
        "octave_table_db": [analysis.band_mean_db(smoothed, f / math.sqrt(2.0), f * math.sqrt(2.0)) - reference_db
                            for f in OCTAVE_CENTRES],
        "t20_ms": reverberation_time_ms(decay_db, sample_rate),
        "t30_ms": reverberation_time_ms(decay_db, sample_rate, -5.0, -35.0),
        "decay_at_fade_db": decay_db[fade_start],
        "fade_energy_fraction": sum(v * v for v in impulse[fade_start:]) / total_energy,
        "last_sample_dbfs": amplitude_db(impulse[-1]),
        "peak_dbfs": amplitude_db(max(abs(v) for v in impulse)),
        "mean_sample": sum(impulse) / num_samples,
        "dc_relative_db": to_db(power[0] / max(power)) if power[0] > 0.0 else -300.0,
        "polarity": 1 if impulse[onset_index] > 0.0 else -1,
        "onset_index": onset_index,
        "energy_gain_db": 20.0 * math.log10(energy_gain),
        "loudness_gain_db": 20.0 * math.log10(loudness_gain),
        "loudness_shipped_db": shipped,
        "loudness_energy_mode_db": {k: v + 20.0 * math.log10(energy_gain) for k, v in shipped.items()},
        "loudness_loudness_mode_db": {k: v + 20.0 * math.log10(loudness_gain) for k, v in shipped.items()},
        "_impulse": impulse,
        "_smoothed": smoothed,
    }


def spectral_distance_db(analysis, a, b, low_hz, high_hz):
    """RMS dB difference of two smoothed curves after removing their level offset.

    Level is removed because the engine level-matches at load; what is left is
    how differently the two are voiced, which is the only thing that makes a
    slot worth having.
    """
    bins = [k for k in range(1, analysis.num_bins) if low_hz <= analysis.frequencies[k] <= high_hz]
    left = [to_db(a["_smoothed"][k]) for k in bins]
    right = [to_db(b["_smoothed"][k]) for k in bins]
    offset = sum(u - v for u, v in zip(left, right)) / len(bins)
    return math.sqrt(sum((u - v - offset) ** 2 for u, v in zip(left, right)) / len(bins))


def blend_coherence(analysis, a, b, source_name):
    """Loudness of an aligned 50/50 blend against a perfectly coherent sum.

    Crossfade blend sums the two convolver branches after IrAlignment has put
    slot B's onset on slot A's, so this mirrors what the engine does: align,
    level-match, sum at 0.5 each.
    """
    left, right = a["_impulse"], b["_impulse"]
    lag, _ = cross_correlation_peak(left, right)
    length = max(len(left), len(right))

    aligned_a = left + [0.0] * (length - len(left))
    aligned_b = [0.0] * length
    for i in range(len(right)):
        target = i - lag
        if 0 <= target < length:
            aligned_b[target] = right[i]

    gain_a = NORMALISATION_REFERENCE_AMPLITUDE / math.sqrt(energy(aligned_a))
    gain_b = NORMALISATION_REFERENCE_AMPLITUDE / math.sqrt(energy(aligned_b))
    aligned_a = [v * gain_a for v in aligned_a]
    aligned_b = [v * gain_b for v in aligned_b]
    summed = [0.5 * (u + v) for u, v in zip(aligned_a, aligned_b)]

    loudness_a = analysis.loudness_db(power_spectrum(aligned_a), source_name)
    loudness_b = analysis.loudness_db(power_spectrum(aligned_b), source_name)
    loudness_sum = analysis.loudness_db(power_spectrum(summed), source_name)

    coherent = to_db(0.25 * (10.0 ** (loudness_a / 20.0) + 10.0 ** (loudness_b / 20.0)) ** 2)
    incoherent = to_db(0.25 * (10.0 ** (loudness_a / 10.0) + 10.0 ** (loudness_b / 10.0)))
    return {"lag_samples": lag, "sum_db": loudness_sum,
            "coherent_db": coherent, "incoherent_db": incoherent,
            "loss_vs_coherent_db": loudness_sum - coherent}


# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------

def family_spread(measurements, mode_key, source_name, family):
    values = [m[mode_key][source_name] for m in measurements if m["family"] == family]
    return max(values) - min(values)


def report(measurements, analysis, blends):
    print("The nine bundled cabinets, measured from the shipped bytes")
    print("Band edges are referenced to each IR's own %g Hz - %g Hz mean on a 1/%d-octave"
          % (REFERENCE_BAND_HZ[0], REFERENCE_BAND_HZ[1], SMOOTHING_FRACTION))
    print("smoothed magnitude response, not to its highest single point.")
    print()
    header = "%-20s %6s %7s %8s %8s %8s %8s %7s %7s %7s %8s %4s"
    print(header % ("id", "taps", "len ms", "-10 lo", "-10 hi", "presence",
                    "lift dB", "2-5k", "5-10k", "T20 ms", "LF dB/oct", "pol"))
    print("-" * 122)
    for m in measurements:
        print(header % (m["id"], m["samples"], "%.1f" % m["length_ms"],
                        "%.0f Hz" % m["edge_lo_10db_hz"], "%.0f Hz" % m["edge_hi_10db_hz"],
                        "%.0f Hz" % m["presence_hz"], "%+.1f" % m["presence_lift_db"],
                        "%+.1f" % m["presence_tilt_db"], "%+.1f" % m["air_tilt_db"],
                        "%.1f" % m["t20_ms"], "%.1f" % m["lf_slope_db_per_oct"],
                        "%+d" % m["polarity"]))

    print()
    print("Octave-band response, dB relative to each IR's own %g Hz - %g Hz mean"
          % REFERENCE_BAND_HZ)
    print("%-20s %s" % ("id", " ".join("%7s" % ("%gHz" % f if f < 1000 else "%gk" % (f / 1000.0))
                                       for f in OCTAVE_CENTRES)))
    for m in measurements:
        print("%-20s %s" % (m["id"], " ".join("%7.1f" % v for v in m["octave_table_db"])))

    print()
    print("Level match. K-weighted loudness in dB, by source spectrum and by the gain")
    print("match the engine applies at load. Spread is max minus min within the family.")
    for label, key in (("as shipped (never heard: the engine renormalises at load)", "loudness_shipped_db"),
                       ("Energy mode - juce Normalise::yes, the default", "loudness_energy_mode_db"),
                       ("Loudness mode - IrLoudness K-weighted", "loudness_loudness_mode_db")):
        print()
        print("  %s" % label)
        print("  %-22s %10s %10s %10s" % ("source", "guitar", "bass", "set"))
        for name, _, _, _ in SOURCES:
            values = [m[key][name] for m in measurements]
            print("  %-22s %9.2f  %9.2f  %9.2f"
                  % (name, family_spread(measurements, key, name, "guitar"),
                     family_spread(measurements, key, name, "bass"),
                     max(values) - min(values)))

    print()
    print("Structural variety. Low-frequency roll-off at each cabinet's own -3 dB corner:")
    print("a dipole is first order, a sealed box second, a vented box fourth.")
    print("%-20s %-12s %10s %12s" % ("id", "enclosure", "-3 dB lo", "dB/octave"))
    for m in measurements:
        print("%-20s %-12s %9.0f Hz %11.1f" % (m["id"], m["enclosure"],
                                               m["edge_lo_3db_hz"], m["lf_slope_db_per_oct"]))

    print()
    print("Pairs. Does each name's claim survive measurement?")
    index = {m["id"]: m for m in measurements}
    for a_id, b_id, claim in PAIRS:
        a, b = index[a_id], index[b_id]
        octaves = math.log2(b["edge_hi_10db_hz"] / a["edge_hi_10db_hz"])
        print("  %-20s -> %-20s (%s)" % (a_id, b_id, claim))
        print("     -10 dB high edge %5.0f -> %5.0f Hz (%+.2f octaves)   2-5 kHz %+5.1f -> %+5.1f dB"
              % (a["edge_hi_10db_hz"], b["edge_hi_10db_hz"], octaves,
                 a["presence_tilt_db"], b["presence_tilt_db"]))
        print("     T20 %6.1f -> %6.1f ms (x%.1f)   proximity index %+5.2f -> %+5.2f dB"
              % (a["t20_ms"], b["t20_ms"], b["t20_ms"] / a["t20_ms"],
                 a["proximity_index_db"], b["proximity_index_db"]))
        blend = blends[(a_id, b_id)]
        print("     aligned 50/50 blend: lag %d, sum %.2f dB, %+.2f dB against a coherent sum"
              % (blend["lag_samples"], blend["sum_db"], blend["loss_vs_coherent_db"]))

    print()
    print("Redundancy. RMS dB difference of level-matched 1/3-octave curves; the twelve")
    print("closest of the thirty-six pairings. A small number is a slot that buys little.")
    bands = [("LF 80-250", 80.0, 250.0), ("MID 250-1.5k", 250.0, 1500.0),
             ("HF 1.5k-8k", 1500.0, 8000.0), ("ALL 80-8k", 80.0, 8000.0)]
    ranked = sorted(itertools.combinations(measurements, 2),
                    key=lambda ab: spectral_distance_db(analysis, ab[0], ab[1], 80.0, 8000.0))
    print("%-20s %-20s %s" % ("a", "b", " ".join("%13s" % name for name, _, _ in bands)))
    for a, b in ranked[:12]:
        print("%-20s %-20s %s" % (a["id"], b["id"],
                                  " ".join("%13.1f" % spectral_distance_db(analysis, a, b, lo, hi)
                                           for _, lo, hi in bands)))


# ---------------------------------------------------------------------------
# Gate
# ---------------------------------------------------------------------------

def check(measurements, analysis, blends):
    failures = []
    index = {m["id"]: m for m in measurements}

    for m in measurements:
        if m["polarity"] != REQUIRED_POLARITY:
            failures.append("%s: polarity %+d, expected %+d - an inverted IR cancels its pair "
                            "under Crossfade blend and defeats the Morph bulk-delay search"
                            % (m["id"], m["polarity"], REQUIRED_POLARITY))

        if m["decay_at_fade_db"] > MAX_ENERGY_DECAY_AT_FADE_DB:
            failures.append("%s: Schroeder decay only %.1f dB down where the generator's fade "
                            "begins (limit %.1f dB) - the fade would be shaping audible decay"
                            % (m["id"], m["decay_at_fade_db"], MAX_ENERGY_DECAY_AT_FADE_DB))

        slope = m["lf_slope_db_per_oct"]
        enclosure = m["enclosure"]
        if enclosure == "open-back" and not slope <= DIPOLE_MAX_SLOPE_DB_PER_OCT:
            failures.append("%s: claims an open-back dipole but rolls off at %.1f dB/octave "
                            "(a first-order dipole region must stay at or under %.1f)"
                            % (m["id"], slope, DIPOLE_MAX_SLOPE_DB_PER_OCT))
        if enclosure == "sealed" and not (SEALED_SLOPE_RANGE_DB_PER_OCT[0] <= slope <= SEALED_SLOPE_RANGE_DB_PER_OCT[1]):
            failures.append("%s: claims a sealed box but rolls off at %.1f dB/octave, outside "
                            "the second-order range %.1f - %.1f"
                            % (m["id"], slope, *SEALED_SLOPE_RANGE_DB_PER_OCT))
        if enclosure == "ported" and not slope >= PORTED_MIN_SLOPE_DB_PER_OCT:
            failures.append("%s: claims a ported box but rolls off at only %.1f dB/octave "
                            "(a vented alignment is fourth order, at least %.1f)"
                            % (m["id"], slope, PORTED_MIN_SLOPE_DB_PER_OCT))

    for a_id, b_id, claim in PAIRS:
        a, b = index[a_id], index[b_id]
        if claim == "off-axis":
            octaves = math.log2(a["edge_hi_10db_hz"] / b["edge_hi_10db_hz"])
            if octaves < MIN_EDGE_DARKER_OCTAVES:
                failures.append("%s -> %s claims an off-axis move but is only %.2f octaves "
                                "darker at the -10 dB edge (at least %.2f expected)"
                                % (a_id, b_id, octaves, MIN_EDGE_DARKER_OCTAVES))
            drop = a["presence_tilt_db"] - b["presence_tilt_db"]
            if drop < MIN_EDGE_PRESENCE_DROP_DB:
                failures.append("%s -> %s claims an off-axis move but loses only %.1f dB of "
                                "2-5 kHz (at least %.1f expected)"
                                % (a_id, b_id, drop, MIN_EDGE_PRESENCE_DROP_DB))
        if claim == "room":
            ratio = b["t20_ms"] / a["t20_ms"]
            if ratio < MIN_ROOM_DECAY_RATIO:
                failures.append("%s -> %s claims a room but decays only %.1fx longer "
                                "(at least %.1fx expected)"
                                % (a_id, b_id, ratio, MIN_ROOM_DECAY_RATIO))

        blend = blends[(a_id, b_id)]
        if blend["loss_vs_coherent_db"] < -MAX_BLEND_COHERENCE_LOSS_DB:
            failures.append("%s + %s: an aligned 50/50 blend loses %.2f dB against a coherent "
                            "sum (limit %.2f) - the pair is combing"
                            % (a_id, b_id, -blend["loss_vs_coherent_db"], MAX_BLEND_COHERENCE_LOSS_DB))

    for family, source_name in VERDICT_SOURCES.items():
        for key, label in (("loudness_energy_mode_db", "Energy"), ("loudness_loudness_mode_db", "Loudness")):
            spread = family_spread(measurements, key, source_name, family)
            if spread > MAX_FAMILY_LEVEL_SPREAD_DB:
                failures.append("%s family spreads %.2f dB in %s mode against %s (ratchet %.2f) - "
                                "auditioning one cabinet after another would jump in level"
                                % (family, spread, label, source_name, MAX_FAMILY_LEVEL_SPREAD_DB))
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", help="directory holding the .wav files and manifest.json")
    parser.add_argument("--json", help="write every measurement to this path")
    parser.add_argument("--check", action="store_true",
                        help="exit non-zero if any measured property breaks a threshold")
    args = parser.parse_args()

    with open(os.path.join(args.directory, "manifest.json")) as handle:
        manifest = json.load(handle)

    analysis = Analysis(48000.0)
    order = [entry for entry in manifest["irs"] if entry["family"] == "guitar"] + \
            [entry for entry in manifest["irs"] if entry["family"] == "bass"]
    measurements = [measure(entry, args.directory, analysis) for entry in order]

    blends = {}
    for a_id, b_id, _ in PAIRS:
        index = {m["id"]: m for m in measurements}
        family = index[a_id]["family"]
        blends[(a_id, b_id)] = blend_coherence(analysis, index[a_id], index[b_id],
                                               VERDICT_SOURCES[family])

    report(measurements, analysis, blends)

    if args.json:
        payload = {"irs": [{k: v for k, v in m.items() if not k.startswith("_")} for m in measurements],
                   "blends": {"%s+%s" % key: value for key, value in blends.items()}}
        with open(args.json, "w") as handle:
            json.dump(payload, handle, indent=2, sort_keys=True)

    failures = check(measurements, analysis, blends)
    print()
    if failures:
        for failure in failures:
            print("FAIL: %s" % failure)
        print("FAIL: %d measured property/properties out of bounds" % len(failures))
        return 1 if args.check else 0

    print("PASS: %d cabinets measured, every set-level property inside its bound"
          % len(measurements))
    return 0


if __name__ == "__main__":
    sys.exit(main())
