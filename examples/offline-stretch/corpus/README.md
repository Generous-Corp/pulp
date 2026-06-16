# OfflineStretch test corpus

Test material for the offline time-stretch / pitch engine
(`pulp::signal::OfflineStretch`). See
`planning/Sampler-Offline-Stretch-Build-Plan.md` §6.

## Layout

```
corpus/
├── synthetic/   generated, deterministic fixtures (gitignored — regenerate)
└── musical/     real loops, user-supplied (gitignored — see below)
```

## synthetic/ — generated, not committed

Deterministic fixtures with exactly-known pitch and onset positions, so
correctness and metrics tests are unambiguous. Regenerate any time:

```bash
python3 examples/offline-stretch/tools/make_corpus.py
```

This writes 32-bit float WAVs at 48 kHz (the engine's native format) plus a
`MANIFEST.tsv`. The generator uses only the Python standard library and no
RNG, so output is byte-identical every run — suitable for seeding regression
baselines. The binaries are **gitignored**: they are cheap to regenerate and we
don't commit generated audio.

Contents: tonal references (`sine_440_*`), a transient reference with exact beat
onsets (`clicks_120bpm`), a broadband `logsweep_20_20k`, safety inputs
(`silence`, `dc`, `fullscale`), and BPM-detection targets at 60/90/120/174.

## musical/ — real loops, user-supplied

Licensed musical audio cannot be generated or committed here. To exercise the
quality metrics and listening checkpoints (plan §6), drop WAV/AIFF loops into
`musical/` covering, at minimum:

- acoustic drum loops — one sparse, one busy
- an electronic loop with a long 808 tail (low-end phase coherence)
- a hi-hat-heavy loop
- a vocal phrase (formant validation)
- a bass line
- a full mix

Tests that require musical material **skip with a clear reason** when `musical/`
is empty, so the suite stays green on a fresh checkout. The synthetic fixtures
cover all correctness (length, null, determinism, safety) and the transient/
pitch metrics on known-onset material without any user audio.

## Running the scoreboard (Pulp vs Rubber Band R3)

`tools/capture_baseline.py` renders the corpus (synthetic + every `musical/*.wav`)
through Pulp's `stretchcli` and — when a `rubberband` CLI is on PATH — through
**Rubber Band R3** (`rubberband -3`) at the same ratios, and scores both with the
reference-free probes in `metrics.py`. Rubber Band is a *benchmark only* (GPL —
never linked or vendored; see the clean-room ledger in
`planning/2026-06-16-offline-stretch-beat-r3-plan.md`).

```bash
# Optional but recommended — the R3 comparison lane:
brew install rubberband            # provides the `rubberband` CLI
# The discriminating metrics (attack sharpness, spectral flatness, crest) need
# numpy; the harness still runs without it and reports those as "skipped".
python3 -m venv .venv && .venv/bin/pip install numpy

.venv/bin/python tools/capture_baseline.py \
  ../../build/examples/offline-stretch/stretchcli corpus/synthetic /tmp/baseline.json
```

Without `rubberband` the R3 lane is reported `deferred`; the Pulp baseline is
still captured. The objective probes are **reference-free by design** —
PEAQ/ViSQOL are invalid for time-stretch because no length-aligned reference
exists; primary quality judgement is blind MUSHRA listening.
