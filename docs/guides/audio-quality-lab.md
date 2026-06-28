# Audio Quality Lab

The Audio Quality Lab is a perception-aware, **offline** developer/CI tool that lets an
agent (not just a human ear) detect subtle DSP artifacts — transient smear, brightness
loss, metallic high-frequency sizzle, graininess — that otherwise require an A/B listen.
It compares a candidate render against a reference and reports, per artifact, whether the
candidate got *worse* and where.

It is a **developer/CI tool, not an end-user plugin feature** — it never links into the
MIT core or a shipped plugin (FFT/analysis stays tool-side). It is additive and opt-in:
Pulp's basic audio tests keep working with zero new dependencies.

## Who it's for

- **Tuning Pulp's own DSP** (and agents doing it) — close the A/B loop without a human
  listening on every iteration.
- **Plugin/app developers building on Pulp** — fine-tune or regression-guard *your own*
  sounds with the same "did this change make it sound worse?" answer.

## How it works

Each run flows through pure, testable stages:

```
generate / load → level-match → align → detect → report.json
```

- **Level-match first** — every comparison normalizes the candidate's loudness to the
  reference, so loudness never decides an A/B.
- **Align before detect** — reference and candidate can differ in length and latency; an
  onset map (and local cross-correlation) aligns them before any detector runs. Sustained
  material uses identity alignment instead.
- **Detect** — independent analyzers each measure one artifact and emit a scalar plus a
  localized worst-region timestamp.
- **Report** — a single JSON verdict, plus optional listenable clips and a re-derivable
  provenance record.

## Detectors

| Detector | Catches |
|----------|---------|
| `transient_sharpness` | percussion attack smear ("compressed" drums) |
| `spectral_centroid` | brightness loss / dulling |
| `hf_fizz` | added metallic high-frequency sizzle |
| `spectral_flux` | graininess / temporal instability (sustained material) |

Each detector fires on its own artifact and stays quiet on the others and on an identity
render. Detectors are validated **non-circularly** — not only against synthetic
degradations but against the output of an independent textbook phase vocoder and against
the real Pulp stretch engine.

## Install + run (opt-in)

```bash
cd tools/audio/quality-lab
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt          # numpy + soundfile (permissive); pytest to test

# run all detectors on a synthetic case and export listenable clips
python -m quality_lab.cli run --case drum  --degradation smear --out-dir out
python -m quality_lab.cli run --case tonal --degradation grainy

# validate the real stretch engine (needs a built stretchcli)
python -m quality_lab.cli engine --ratio 2.0 --character clean
python -m quality_lab.cli engine --input yourfile.wav --character varispeed   # any real WAV

# regression gate: did an engine change make it worse?
python -m quality_lab.cli engine-baseline

# manage the versioned, license-guarded corpus
python -m quality_lab.cli corpus list
python -m quality_lab.cli corpus add --file vocal.wav --name vocal1 \
    --class vocal --license CC0 --expect "graininess on sustained notes"

pytest tests/ -q
```

The lab's pytest suite is intentionally **not** wired into the default `ctest` — the
lab's dependencies are opt-in and basic testing stays dependency-free.

## How it stays trustworthy

- **Coverage / confidence** — each detector reports how many onsets it actually measured;
  a "clean" verdict with low coverage reads `UNCERTAIN`, never a silent pass.
- **Real-engine validation** — the detectors run against the actual product stretch
  engine, and a committed baseline flags when an engine change deviates from it.
- **Provenance** — each report records the engine commit, recipe, and determinism context,
  so a render you liked maps back to how it was made.
- **License fence** — heavier or copyleft tools (perceptual models, reference stretchers)
  are reached only via an explicit env-path and never bundled; the committed corpus stays
  permissively licensed.

## Relationship to the existing audio harness

This builds on, and does not replace, the offline audio-observability harness in
[testing.md](testing.md). That measures presence / level / THD / response; the Quality Lab
adds *reference-vs-candidate perceptual artifact* detection for fine-tuning. See
`tools/audio/quality-lab/README.md` for the module map and current detector status.
