# Audio Quality Lab

The Audio Quality Lab is a perception-aware, **offline** developer/CI tool that lets
an agent (not just a human ear) detect subtle DSP artifacts — transient smear, seam
clicks, timing drift — that today require an A/B listen. It is the rigor upgrade that
sits *beside* the basic A/B tooling (`examples/offline-stretch/eval/`,
`pulp audio validate`); the basics keep working with zero new dependencies.

It is a **developer/CI tool, not an end-user plugin feature** — it never links into the
MIT core or a shipped plugin (FFT/analysis stays tool-side). Full design lives in the
private planning doc `2026-06-26-audio-quality-lab-perceptual-harness.md`.

## Who it's for

- **Pulp's own DSP tuning** (and agents doing it) — close the A/B loop without a human
  listening on every iteration.
- **Plugin/app developers building on Pulp** — fine-tune or regression-guard *your own*
  sounds with the same "did this change make it sound worse?" answer.

## Status — P0a (proving the architecture)

The first slice proves the pipeline end to end on a synthetic drum break:

```
generate → level-match → onset-map align → transient-sharpness detector → report.json
```

The gate it had to pass: **localize** a known transient smear (within ±20 ms) AND stay
**quiet** on an identity render. It does, and — crucially — the transient detector is
validated **non-circularly** against an *independent* textbook phase vocoder
(`reference_pv.py`), firing hard (scalar ≈ 1.0) on real PV attack smear. That is
evidence it catches the real documented artifact, not a kernel matched to itself.

## Install + run (opt-in)

```bash
cd tools/audio/quality-lab
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt          # numpy + soundfile (permissive); pytest to test

python -m quality_lab.cli run-p0a --mode bad   # smeared candidate → FIRES + localizes
python -m quality_lab.cli run-p0a --mode good  # identity render   → CLEAN
pytest tests/ -q
```

The lab's pytest suite is intentionally **not** wired into the default `ctest` — the
lab's dependencies are opt-in and basic testing stays dependency-free.

## How it stays trustworthy

- **Level-match first** — every comparison normalizes candidate RMS to the reference
  (the skill's rule #1) so loudness never decides an A/B.
- **Alignment before detection** — reference and candidate differ in length/latency;
  an onset map (and local cross-correlation) aligns them before any detector runs.
- **Coverage/confidence** — each detector reports how many onsets it actually measured;
  a "clean" verdict with low coverage reads `UNCERTAIN`, never a silent pass.
- **Provenance** — each report records the engine commit, recipe, and determinism
  context so a good-sounding render is re-derivable ("same recipe" tier).

## Relationship to the existing audio harness

This builds on, and does not replace, the offline audio-observability harness
documented in [testing.md](testing.md) and the `audio-harness` skill
(`pulp::audio-analysis`, `pulp audio validate`). Those measure presence / level / THD /
response; the Quality Lab adds *reference-vs-candidate perceptual artifact* detection
for fine-tuning. See `tools/audio/quality-lab/README.md` for the module map and the
honest status of deferred detectors.
