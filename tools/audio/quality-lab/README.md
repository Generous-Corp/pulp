# Audio Quality Lab

A perception-aware, offline harness so an **agent** (not just a human ear) can detect
the subtle DSP artifacts — transient smear, seam clicks, sub-band wobble — that today
gate on an A/B listen. It is an **additive, opt-in developer/CI tool**: Pulp's basic
audio tests keep working with zero new dependencies; install this lab's deps only to
use it.

Full design: `planning/2026-06-26-audio-quality-lab-perceptual-harness.md` (private
planning submodule). It composes with the existing
`examples/offline-stretch/eval/` A/B toolkit — basic A/B stands alone; this is the
rigor upgrade that consumes the same renders.

## Status — P0a (the go/no-go slice)

This is the smallest end-to-end slice that proves the architecture:

```
generate → level-match → onset-map align → transient-sharpness detector → report.json
```

It must **localize** a known transient smear (within ±20 ms) AND stay **quiet** on an
identity render. If it can't, the rest of the plan doesn't get built.

## Install (opt-in)

```bash
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt   # numpy + soundfile (both permissive); pytest to run tests
```

## Run

```bash
# from this directory
python -m quality_lab.cli run-p0a --mode bad  --out report.json   # smeared candidate → FIRES + localizes
python -m quality_lab.cli run-p0a --mode good                     # identity candidate → CLEAN
```

## Test

```bash
pytest tests/ -q
```

The lab's pytest suite is **not** wired into the default `ctest` run — the lab's deps
are opt-in, and basic testing must stay dependency-free (the plan's additive-by-default
rule). The first-class `pulp audio quality` CLI verb and broader detector suite arrive
in later phases.

## Layout (the stable seams are the schemas, not the code)

| Module | Role |
|--------|------|
| `quality_lab/schema.py` | `QualityCase`, report envelope, detector result — the public API |
| `quality_lab/audio_io.py` | WAV load/save + RMS level-match (rule #1) |
| `quality_lab/generate.py` | deterministic, self-labeling drum-break + smear degradation |
| `quality_lab/align.py` | onset detection + onset-map (alignment runs before detectors) |
| `quality_lab/detectors/` | one detector = one small module (`transient_sharpness.py`) |
| `quality_lab/provenance.py` | re-derivable provenance block (§7.1) |
| `quality_lab/pipeline.py` | pure stages: generate → level-match → align → detect → report |
| `quality_lab/cli.py` | parse + dispatch only |

See `NOTICE.md` for third-party attribution and the license fence.
