#!/usr/bin/env python3
"""Measure bundled impulse responses as signals, not just as files.

Reads every 24-bit WAV in a directory back off disk - i.e. the exact bytes that
ship - and asserts the properties a cabinet IR has to have before it is safe to
convolve a user's track with it:

  * format          mono, 24-bit PCM, at the expected sample rate
  * no clipping     no sample sitting on the full-scale code
  * no DC offset    |H(0)| far below the peak of the magnitude response, so the
                    convolver cannot walk a signal off centre
  * normalisation   max |H(f)| == 0 dBFS, so no sine at any frequency can leave
                    the convolver hotter than it entered
  * sane decay      a monotone Schroeder decay, a measurable T20, and a tail
                    that has actually faded to nothing by the last sample
                    rather than being cut off mid-ring
  * bandwidth       -3 dB and -10 dB corners, plus a per-octave response table,
                    so a voicing can be argued about with numbers

Exit status is non-zero if any file fails a threshold, so this runs in CI as a
gate rather than as a report nobody reads.

Standard library only - see cabsynth.py for why.
"""

from __future__ import annotations

import argparse
import cmath
import hashlib
import glob
import json
import math
import os
import struct
import sys

# --- thresholds ------------------------------------------------------------
# Every one of these is a property of the generator's output, verified after
# the fact against the file on disk rather than trusted from the code that
# wrote it.
MAX_PEAK_RESPONSE_DEVIATION_DB = 0.05   # normalisation target: 0 dB
MAX_DC_RELATIVE_DB = -60.0              # |H(0)| relative to peak |H(f)|
MAX_DC_OFFSET = 1.0e-4                  # mean sample value
MAX_TAIL_END_DBFS = -90.0               # last sample: the fade must have closed
MIN_RT_MS = 0.3
MAX_RT_MS = 400.0
FFT_SIZE = 65536

OCTAVE_CENTRES = [31.5, 63.0, 125.0, 250.0, 500.0, 1000.0,
                  2000.0, 4000.0, 8000.0, 16000.0]


def read_wav(path):
    with open(path, "rb") as handle:
        data = handle.read()

    if data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise ValueError("%s: not a RIFF/WAVE file" % path)

    pos = 12
    fmt = None
    payload = None
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
    if audio_format != 1:
        raise ValueError("%s: not PCM (format %d)" % (path, audio_format))
    if bits != 24:
        raise ValueError("%s: expected 24-bit, got %d" % (path, bits))

    full_scale = float(1 << 23)
    samples = []
    clipped = 0
    for offset in range(0, len(payload) - 2, 3):
        raw = payload[offset] | (payload[offset + 1] << 8) | (payload[offset + 2] << 16)
        if raw & 0x800000:
            raw -= 0x1000000
        if abs(raw) >= (1 << 23) - 1:
            clipped += 1
        samples.append(raw / full_scale)

    return {
        "path": path,
        "channels": channels,
        "sample_rate": sample_rate,
        "bits": bits,
        "samples": samples,
        "clipped": clipped,
    }


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
        step = cmath.exp(complex(0.0, angle))
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


def to_db(value, floor=1.0e-12):
    return 20.0 * math.log10(max(abs(value), floor))


def band_rms(magnitude, centre_hz, bin_hz, fraction):
    """RMS magnitude across a 1/`fraction`-octave band centred on `centre_hz`."""
    half_width = 2.0 ** (1.0 / (2.0 * fraction))
    low = max(0, int(math.floor((centre_hz / half_width) / bin_hz)))
    high = min(len(magnitude) - 1, int(math.ceil((centre_hz * half_width) / bin_hz)))
    if high < low:
        low = high = min(len(magnitude) - 1, max(0, int(round(centre_hz / bin_hz))))
    window = magnitude[low:high + 1]
    return math.sqrt(sum(v * v for v in window) / len(window))


def smooth_fractional_octave(magnitude, fraction):
    """1/`fraction`-octave RMS smoothing of a magnitude spectrum, bin by bin."""
    n = len(magnitude)
    half_width = 2.0 ** (1.0 / (2.0 * fraction))
    # Running sum of squares, so each band average is O(1) rather than O(width).
    prefix = [0.0] * (n + 1)
    for i, value in enumerate(magnitude):
        prefix[i + 1] = prefix[i] + value * value
    out = [0.0] * n
    for k in range(n):
        low = max(0, int(math.floor(k / half_width)))
        high = min(n - 1, int(math.ceil(k * half_width)))
        count = high - low + 1
        out[k] = math.sqrt((prefix[high + 1] - prefix[low]) / count)
    return out


def measure(wav):
    samples = wav["samples"]
    sample_rate = wav["sample_rate"]
    n = len(samples)

    for value in samples:
        if math.isnan(value) or math.isinf(value):
            raise ValueError("%s: non-finite sample" % wav["path"])

    peak = max(abs(v) for v in samples)
    rms = math.sqrt(sum(v * v for v in samples) / n)
    dc_offset = sum(samples) / n

    spectrum = fft([complex(v, 0.0) for v in samples] + [0j] * (FFT_SIZE - n))
    half = FFT_SIZE // 2
    magnitude = [abs(spectrum[k]) for k in range(half + 1)]
    peak_response = max(magnitude)
    bin_hz = sample_rate / float(FFT_SIZE)

    # Corners and the octave table are read off a 1/3-octave RMS-smoothed
    # curve, not the raw bins. A cabinet response is a comb of narrow breakup
    # peaks and nulls; walking outwards from a raw-spectrum maximum finds the
    # skirt of one breakup peak and calls it the cabinet's bandwidth, which is
    # measurement theatre. Smoothing first is what an acoustic measurement
    # would do, and it makes the numbers mean what a reader assumes.
    smoothed = smooth_fractional_octave(magnitude, fraction=3)
    smoothed_peak = max(smoothed)
    smoothed_peak_bin = smoothed.index(smoothed_peak)

    def response_at(freq_hz):
        return to_db(band_rms(magnitude, freq_hz, bin_hz, fraction=1) / smoothed_peak)

    def corner(threshold_db, direction):
        target = smoothed_peak * (10.0 ** (threshold_db / 20.0))
        index = smoothed_peak_bin
        while 0 < index < half:
            index += direction
            if smoothed[index] < target:
                return index * bin_hz
        return None

    # Schroeder backward energy integration -> T20, extrapolated to the usual
    # 60 dB convention.
    energy = 0.0
    schroeder = [0.0] * n
    for i in range(n - 1, -1, -1):
        energy += samples[i] * samples[i]
        schroeder[i] = energy
    total = schroeder[0] or 1.0
    curve = [10.0 * math.log10(max(v / total, 1.0e-20)) for v in schroeder]

    def first_below(level_db):
        for i, value in enumerate(curve):
            if value <= level_db:
                return i
        return None

    i5 = first_below(-5.0)
    i25 = first_below(-25.0)
    rt_ms = None
    if i5 is not None and i25 is not None and i25 > i5:
        rt_ms = 3.0 * (i25 - i5) / sample_rate * 1000.0

    tail_end_dbfs = to_db(max(abs(v) for v in samples[-8:]))
    last_tenth = samples[int(n * 0.9):]
    tail_energy_fraction = (sum(v * v for v in last_tenth) / (sum(v * v for v in samples) or 1.0))

    return {
        "file": os.path.basename(wav["path"]),
        "sample_rate": sample_rate,
        "channels": wav["channels"],
        "bits": wav["bits"],
        "length_samples": n,
        "length_ms": round(1000.0 * n / sample_rate, 2),
        "clipped_samples": wav["clipped"],
        "peak_sample": round(peak, 6),
        "peak_dbfs": round(to_db(peak), 2),
        "rms_dbfs": round(to_db(rms), 2),
        "dc_offset": dc_offset,
        "dc_relative_db": round(to_db(magnitude[0] / peak_response), 2),
        "peak_response_dbfs": round(to_db(peak_response), 3),
        "peak_response_hz": round(smoothed_peak_bin * bin_hz, 1),
        "minus3db_low_hz": corner(-3.0, -1),
        "minus3db_high_hz": corner(-3.0, +1),
        "minus10db_low_hz": corner(-10.0, -1),
        "minus10db_high_hz": corner(-10.0, +1),
        "octave_response_db": {str(f): round(response_at(f), 2) for f in OCTAVE_CENTRES},
        "t20_rt_ms": None if rt_ms is None else round(rt_ms, 3),
        "tail_end_dbfs": round(tail_end_dbfs, 2),
        "tail_energy_last_10pct": tail_energy_fraction,
    }


def check(result, expected_rate):
    failures = []
    if result["channels"] != 1:
        failures.append("expected mono, got %d channels" % result["channels"])
    if expected_rate and result["sample_rate"] != expected_rate:
        failures.append("expected %d Hz, got %d Hz" % (expected_rate, result["sample_rate"]))
    if result["clipped_samples"] != 0:
        failures.append("%d clipped samples" % result["clipped_samples"])
    if abs(result["peak_response_dbfs"]) > MAX_PEAK_RESPONSE_DEVIATION_DB:
        failures.append("peak magnitude response %.3f dBFS, expected 0 +/- %.2f"
                        % (result["peak_response_dbfs"], MAX_PEAK_RESPONSE_DEVIATION_DB))
    if result["dc_relative_db"] > MAX_DC_RELATIVE_DB:
        failures.append("DC response %.2f dB relative to peak, limit %.1f"
                        % (result["dc_relative_db"], MAX_DC_RELATIVE_DB))
    if abs(result["dc_offset"]) > MAX_DC_OFFSET:
        failures.append("DC offset %.3e, limit %.1e" % (result["dc_offset"], MAX_DC_OFFSET))
    if result["peak_sample"] >= 1.0:
        failures.append("peak sample %.6f is at or over full scale" % result["peak_sample"])
    if result["tail_end_dbfs"] > MAX_TAIL_END_DBFS:
        failures.append("IR is still ringing at %.1f dBFS on its last samples (limit %.1f)"
                        % (result["tail_end_dbfs"], MAX_TAIL_END_DBFS))
    if result["t20_rt_ms"] is None:
        failures.append("no measurable decay time - the Schroeder curve never reaches -25 dB")
    elif not (MIN_RT_MS <= result["t20_rt_ms"] <= MAX_RT_MS):
        failures.append("RT(T20) %.3f ms outside [%.1f, %.1f]"
                        % (result["t20_rt_ms"], MIN_RT_MS, MAX_RT_MS))
    return failures


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("directory", help="directory containing the .wav impulse responses")
    parser.add_argument("--expected-sample-rate", type=int, default=48000)
    parser.add_argument("--json", help="write the full measurements as JSON")
    parser.add_argument("--min-files", type=int, default=1,
                        help="fail if fewer than this many IRs were found")
    parser.add_argument("--expect-manifest",
                        help="cross-check every file's SHA-256 against a cabsynth.py "
                             "manifest, so the committed provenance record cannot drift "
                             "away from the committed audio")
    args = parser.parse_args(argv)

    paths = sorted(glob.glob(os.path.join(args.directory, "*.wav")))
    if len(paths) < args.min_files:
        print("FAIL: found %d IR(s) in %s, expected at least %d"
              % (len(paths), args.directory, args.min_files), file=sys.stderr)
        return 1

    manifest_digests = {}
    if args.expect_manifest:
        with open(args.expect_manifest) as handle:
            manifest = json.load(handle)
        manifest_digests = {entry["file"]: entry["sha256"] for entry in manifest["irs"]}

    results = []
    all_failures = {}
    for path in paths:
        result = measure(read_wav(path))
        results.append(result)
        failures = check(result, args.expected_sample_rate)
        if manifest_digests:
            name = os.path.basename(path)
            expected = manifest_digests.get(name)
            actual = hashlib.sha256(open(path, "rb").read()).hexdigest()
            if expected is None:
                failures.append("not listed in the manifest - undocumented asset")
            elif expected != actual:
                failures.append("sha256 %s does not match the manifest's %s"
                                % (actual[:16], expected[:16]))
        if failures:
            all_failures[result["file"]] = failures

    def hz(value):
        return "-" if value is None else "%.0f" % value

    header = ("%-34s %5s %8s %8s %8s %9s %8s %8s %8s %8s %8s %8s"
              % ("file", "len", "peak dB", "rms dB", "maxH dB", "DC rel dB",
                 "-3dB lo", "-3dB hi", "-10dB lo", "-10dB hi", "RT ms", "end dB"))
    print(header)
    print("-" * len(header))
    for r in results:
        print("%-34s %5d %8.2f %8.2f %8.3f %9.1f %8s %8s %8s %8s %8s %8.1f"
              % (r["file"], r["length_samples"], r["peak_dbfs"], r["rms_dbfs"],
                 r["peak_response_dbfs"], r["dc_relative_db"],
                 hz(r["minus3db_low_hz"]), hz(r["minus3db_high_hz"]),
                 hz(r["minus10db_low_hz"]), hz(r["minus10db_high_hz"]),
                 "-" if r["t20_rt_ms"] is None else "%.2f" % r["t20_rt_ms"],
                 r["tail_end_dbfs"]))

    print()
    print("Octave-band magnitude response, dB relative to each IR's own peak:")
    print("%-34s %s" % ("file", " ".join("%7s" % ("%gHz" % f) for f in OCTAVE_CENTRES)))
    for r in results:
        print("%-34s %s" % (r["file"],
                            " ".join("%7.1f" % r["octave_response_db"][str(f)]
                                     for f in OCTAVE_CENTRES)))

    if args.json:
        with open(args.json, "w") as handle:
            json.dump({"thresholds": {
                "max_peak_response_deviation_db": MAX_PEAK_RESPONSE_DEVIATION_DB,
                "max_dc_relative_db": MAX_DC_RELATIVE_DB,
                "max_dc_offset": MAX_DC_OFFSET,
                "max_tail_end_dbfs": MAX_TAIL_END_DBFS,
                "t20_ms_range": [MIN_RT_MS, MAX_RT_MS],
            }, "results": results}, handle, indent=2, sort_keys=True)
            handle.write("\n")

    print()
    if all_failures:
        for name, failures in all_failures.items():
            for failure in failures:
                print("FAIL %s: %s" % (name, failure), file=sys.stderr)
        return 1

    if manifest_digests:
        missing = sorted(set(manifest_digests) - {r["file"] for r in results})
        if missing:
            for name in missing:
                print("FAIL manifest: %s is documented but not present" % name, file=sys.stderr)
            return 1

    print("PASS: %d impulse response(s) verified" % len(results))
    return 0


if __name__ == "__main__":
    sys.exit(main())
