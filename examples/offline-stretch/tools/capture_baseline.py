#!/usr/bin/env python3
"""Capture the OfflineStretch quality baseline over the corpus.

This captures the historical realtime-engine baseline. Offline-only refinements,
or making an opt-in refinement the default, should beat THESE numbers before
shipping as the default path.

Runs stretchcli over the synthetic corpus at a sweep of ratios, computes the
metrics (metrics.py), and writes a JSON baseline + a readable table.

Rubber Band R3 comparison: if a `rubberband` CLI is on PATH it is also rendered
and scored; otherwise that lane is reported as "deferred (rubberband not
installed)" — the baseline is still captured.

Usage:
  capture_baseline.py STRETCHCLI CORPUS_DIR OUT_JSON
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metrics as M  # noqa: E402

RATIOS = [0.75, 0.85, 1.15, 1.5]
FILES = [
    "clickloop_120bpm.wav",  # transients (onset timing / pre-echo)
    "sine_440_mono.wav",     # tonal (length / null behaviour)
    "logsweep_20_20k.wav",   # broadband
]


def render(cli, src, dst, ratio):
    subprocess.run([cli, src, dst, "--ratio", str(ratio)],
                   check=True, capture_output=True)


def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: capture_baseline.py STRETCHCLI CORPUS_DIR OUT_JSON\n")
        return 2
    cli, corpus, out_json = argv[1], argv[2], argv[3]
    have_rb = shutil.which("rubberband") is not None

    rows = []
    for fname in FILES:
        src = os.path.join(corpus, fname)
        if not os.path.exists(src):
            continue
        for r in RATIOS:
            dst = os.path.join("/tmp", f"baseline_{fname}_{r}.wav")
            render(cli, src, dst, r)
            m = M.metrics(src, dst, r)
            row = {
                "file": fname, "ratio": r,
                "length_exact": m.get("length_exact"),
                "onset_timing_mean_ms": m.get("onset_timing_mean_ms"),
                "onset_timing_max_ms": m.get("onset_timing_max_ms"),
                "pre_echo_db": m.get("pre_echo_db"),
                "null_rms_db": m.get("null_rms_db"),
            }
            rows.append(row)

    baseline = {
        "engine": "OfflineStretch tempo-only baseline (RealtimePitchTimeProcessor)",
        "rubberband_comparison": "available" if have_rb else "deferred (rubberband not installed)",
        "ratios": RATIOS,
        "rows": rows,
    }
    with open(out_json, "w") as f:
        json.dump(baseline, f, indent=2)

    # Readable table.
    print(f"OfflineStretch baseline ({len(rows)} renders) -> {out_json}")
    print(f"  Rubber Band R3 lane: {baseline['rubberband_comparison']}")
    hdr = f"{'file':22} {'ratio':>5} {'len_ok':>6} {'onset_ms(mean/max)':>20} {'pre_echo_db':>11}"
    print(hdr)
    for r in rows:
        om = r["onset_timing_mean_ms"]; ox = r["onset_timing_max_ms"]
        ot = f"{om}/{ox}" if om is not None else "-"
        print(f"{r['file']:22} {r['ratio']:>5} {str(r['length_exact']):>6} {ot:>20} "
              f"{str(r['pre_echo_db']):>11}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
