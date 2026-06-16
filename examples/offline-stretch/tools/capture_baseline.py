#!/usr/bin/env python3
"""Capture the OfflineStretch quality baseline AND the Rubber Band R3 lane.

Plan: planning/2026-06-16-offline-stretch-beat-r3-plan.md (P0 — harness + R3
baseline). Renders the corpus through Pulp's `stretchcli` and, when a
`rubberband` CLI is on PATH, through Rubber Band R3 (`rubberband -3`) at the same
ratios, scores both with the reference-free probes (metrics.py), and writes a
JSON scoreboard + a readable side-by-side table.

R3 is a *benchmark*, never linked or vendored (it is GPL; see the plan's
clean-room ledger). The point of the scoreboard: replace anecdotes ("the demo
sounds rough") with measured, apples-to-apples numbers before any code change.

Corpus:
  - synthetic named fixtures (exact-onset clicks, tonal sine, broadband sweep);
  - every *.wav under a sibling/child `musical/` dir (user-supplied real loops —
    NOT committed, because licensed audio cannot live in the repo).

Usage:
  capture_baseline.py STRETCHCLI CORPUS_DIR OUT_JSON
"""

from __future__ import annotations

import glob
import json
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import metrics as M  # noqa: E402

RATIOS = [0.75, 0.85, 1.15, 1.5, 2.0]
SYNTHETIC = [
    "clickloop_120bpm.wav",  # transients (onset timing / pre-echo / attack)
    "sine_440_mono.wav",     # tonal (length / null behaviour)
    "logsweep_20_20k.wav",   # broadband (spectral flatness)
]

# The reference-free fields lifted out of metrics() into each scoreboard row.
_FIELDS = ("length_exact", "onset_timing_mean_ms", "onset_timing_max_ms",
           "pre_echo_db", "null_rms_db", "attack_sharpness",
           "spectral_flatness", "crest_db")


def render_pulp(cli, src, dst, ratio):
    subprocess.run([cli, src, dst, "--ratio", str(ratio)],
                   check=True, capture_output=True)


def render_rb(src, dst, ratio):
    # Rubber Band R3 ("Finer" engine), time ratio = output/input duration.
    subprocess.run(["rubberband", "-3", "-t", str(ratio), src, dst],
                   check=True, capture_output=True)


def collect_corpus(corpus):
    """Named synthetic fixtures in `corpus`, plus every *.wav under a `musical/`
    dir (checked at corpus/musical and corpus/../musical)."""
    files = []
    for name in SYNTHETIC:
        p = os.path.join(corpus, name)
        if os.path.exists(p):
            files.append((name, p))
    for musical in (os.path.join(corpus, "musical"),
                    os.path.join(os.path.dirname(os.path.normpath(corpus)), "musical")):
        if os.path.isdir(musical):
            for p in sorted(glob.glob(os.path.join(musical, "*.wav"))):
                files.append((f"musical/{os.path.basename(p)}", p))
            break
    return files


def score(engine, fname, ratio, src, dst):
    m = M.metrics(src, dst, ratio)
    row = {"engine": engine, "file": fname, "ratio": ratio}
    row.update({k: m.get(k) for k in _FIELDS})
    return row


def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: capture_baseline.py STRETCHCLI CORPUS_DIR OUT_JSON\n")
        return 2
    cli, corpus, out_json = argv[1], argv[2], argv[3]
    have_rb = shutil.which("rubberband") is not None
    corpus_files = collect_corpus(corpus)

    rows = []
    for fname, src in corpus_files:
        tag = fname.replace("/", "_")
        for r in RATIOS:
            pulp_dst = os.path.join("/tmp", f"baseline_pulp_{tag}_{r}.wav")
            render_pulp(cli, src, pulp_dst, r)
            rows.append(score("pulp", fname, r, src, pulp_dst))
            if have_rb:
                rb_dst = os.path.join("/tmp", f"baseline_rb_{tag}_{r}.wav")
                render_rb(src, rb_dst, r)
                rows.append(score("rubberband-r3", fname, r, src, rb_dst))

    baseline = {
        "engine": "OfflineStretch (stretchcli) vs Rubber Band R3 benchmark",
        "rubberband_comparison": "rendered + scored" if have_rb
        else "deferred (rubberband not on PATH — `brew install rubberband`)",
        "ratios": RATIOS,
        "corpus_files": [f for f, _ in corpus_files],
        "rows": rows,
    }
    with open(out_json, "w") as f:
        json.dump(baseline, f, indent=2)

    # Readable side-by-side table (pulp vs R3 per file/ratio).
    print(f"OfflineStretch baseline vs Rubber Band R3 ({len(rows)} renders) -> {out_json}")
    print(f"  R3 lane: {baseline['rubberband_comparison']}")
    hdr = (f"{'file':22}{'ratio':>6}{'engine':>15}{'len':>5}"
           f"{'attack↑':>9}{'flat↑':>8}{'crest':>7}{'onset_ms':>10}{'preEcho':>9}")
    print(hdr)
    for r in rows:
        def s(v):
            return "-" if v is None or isinstance(v, str) else v
        print(f"{r['file']:22}{r['ratio']:>6}{r['engine']:>15}"
              f"{str(s(r['length_exact'])):>5}{str(s(r['attack_sharpness'])):>9}"
              f"{str(s(r['spectral_flatness'])):>8}{str(s(r['crest_db'])):>7}"
              f"{str(s(r['onset_timing_mean_ms'])):>10}{str(s(r['pre_echo_db'])):>9}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
