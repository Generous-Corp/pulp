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
