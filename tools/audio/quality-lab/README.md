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

# run all detectors on a degradation and EXPORT listenable artifacts:
python -m quality_lab.cli run --degradation smear --out-dir out --out out/report.json
# writes out/reference.wav, out/candidate.wav, and a ref/candidate clip PAIR around each
# worst region (region-NN-<detector>-<t>.{ref,cand}.wav) so you can hear the artifact.
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
| `quality_lab/detectors/` | one detector = one small module: `transient_sharpness.py`, `spectral_centroid.py` (brightness/dulling), `hf_fizz.py` (metallic HF sizzle) |
| `quality_lab/dsp.py` | shared primitives (high-band, smoothed envelope, normalized local-align) |
| `quality_lab/reference_pv.py` | an INDEPENDENT textbook phase vocoder for non-circular credibility tests |
| `quality_lab/provenance.py` | re-derivable provenance block (§7.1) |
| `quality_lab/pipeline.py` | pure stages: generate → level-match → align → detect → report |
| `quality_lab/cli.py` | parse + dispatch only |

## Validate the REAL Pulp stretch engine (strongest credibility)

The lab can run the detectors against the **actual product engine** (`stretchcli`
driving `pulp::signal::OfflineStretch`), not just the in-tree reference phase vocoder:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF
cmake --build build --target stretchcli
python -m quality_lab.cli engine --ratio 2.0 --character clean      # real engine output
python -m quality_lab.cli engine --ratio 2.0 --character varispeed  # different character
```

This renders a source, stretches it with the real engine, and runs the detectors
against a transient-faithful reference. It catches the engine's documented artifacts —
`clean` smears attacks (transient_sharpness fires 1.0); `varispeed` additionally dulls
brightness (spectral_centroid fires), matching the skill's "slowing down warms + dulls"
note. The engine adapter is opt-in: `stretchcli` is discovered at the build path or via
`PULP_STRETCHCLI`, and skips cleanly when absent (public CI doesn't build it).

### Real audio (any WAV — reference-free)

Real audio has no synthetic "ideal" reference, so the lab checks it **reference-free**:
a faithful time-stretch preserves the *source* spectrum (timing changes, timbre
shouldn't), so the global spectral detectors compare the engine output's LTAS to the
source's.

```bash
python -m quality_lab.cli engine --input yourfile.wav --ratio 2.0 --character varispeed
```

Works on any developer-supplied WAV. The **committed corpus stays synthetic / license-
clean**; real audio is developer-local (your CC0 files, or quick local material). For a
fast license-clean demo on macOS, synthesize real speech with `say`:

```bash
say -o /tmp/vox.aiff "She sells sea shells; the sustained vowel rings on."
python -m quality_lab.cli engine --input /tmp/vox.aiff --character varispeed  # formants drop ~58% -> centroid fires
python -m quality_lab.cli engine --input /tmp/vox.aiff --character clean       # faithful -> CLEAN
```

(`say` voices are Apple-proprietary — fine to *use* locally, do **not** commit the audio.)

## Credibility — why this isn't circular

A detector validated only against a degradation written by the same author is
self-fulfilling. So `transient_sharpness` is also tested against the output of an
**independent textbook phase vocoder** (`reference_pv.py`) — which smears attacks
through genuine STFT resynthesis with no knowledge of the detector. The detector
fires hard (scalar ≈ 1.0) on that real PV smear, and stays clean comparing a PV
render to itself. That is the evidence it catches the *real* documented artifact,
not just a matched kernel. (`tests/test_real_pv_evidence.py`.)

Detectors also report **coverage** (onsets actually measured / offered); a "clean"
verdict with low coverage reads `UNCERTAIN`, never a silent pass.

## Perceptual models (Layer B — opt-in, license-fenced)

A full-reference perceptual MOS predictor is a *coarse global guard* complementary to
the Layer-A detectors — advisory, never a gate. **ViSQOL** (Apache-2.0) is reached
ONLY via an explicit env-path you set, never bundled or auto-downloaded:

```bash
export PULP_VISQOL_BIN=/path/to/visqol   # opt-in; unset → the report's perceptual block is "skipped"
python -m quality_lab.cli run --degradation smear --out-dir out
```

When unset (the default, and always in public CI), the perceptual block degrades to
`skipped` with a reason — never an error. PEAQ / AQUATK (GPL, Tier-3, developer-
supplied) plug in the same way behind their own env-paths; no copyleft code is ever
imported or bundled (see `NOTICE.md` and the plan's §4 license fence).

## Self-describing samples (provenance)

`run --out-dir` writes a `<wav>.provenance.json` sidecar next to each rendered WAV,
carrying the engine commit, recipe, determinism context, and the WAV's SHA-256 — so a
sample you liked maps back to exactly how it was made ("same-recipe" tier), even if
separated from its run folder.

## Detectors

| Detector | Catches | Method | Family |
|----------|---------|--------|--------|
| `transient_sharpness` | PV attack smear ("compressed" drums) | per-onset high-band attack-rise deficit, locally aligned | percussive |
| `spectral_centroid` | brightness loss / dulling (e.g. STN noise-morph) | LTAS centroid shift, candidate vs reference (global) | any |
| `hf_fizz` | added metallic HF sizzle | added HF (>8 kHz) energy fraction vs reference (global) | any |
| `spectral_flux` | graininess / temporal instability | mean energy-normalized spectral flux increase (global) | tonal / sustained |

The spectral detectors are **global** metrics — alignment-free and scale-invariant.
`spectral_flux` belongs to the **tonal** family: on transient-heavy drums the onset
flux dominates and it can't discriminate graininess, so it is exercised on sustained
material (where graininess is actually heard).

## Corpus (P0b — versioned, license-guarded)

The lab has a versioned corpus (`corpus/MANIFEST.json`) of sources by material class,
license, and the artifact each should expose. Two kinds: **generator-backed** (synthetic,
regenerable — committed by recipe, no WAV) and **file-backed** (real audio you supply).

```bash
python -m quality_lab.cli corpus list
python -m quality_lab.cli corpus seed                          # synthetic families
python -m quality_lab.cli corpus add --file vox.wav --name vocal1 \
    --class vocal --license CC0 --expect "graininess on sustained notes" --family tonal
```

**License fence (enforced in code):** `corpus add` rejects any non-permissive license —
the committed corpus stays license-clean (the plan's §4). GPL / proprietary audio, and
R3/PEAQ outputs, stay developer-local and never enter the manifest. File-backed sources
record a SHA-256 (tamper-detected on `materialize`).

## Case families (§3.5 — the harness serves more than drums)

The lab's unit of work is a `QualityCase` with `family` / `reference_policy` /
`alignment_policy` / `detector_tags`, so time-stretch is *one* family, not the
ontology. Two families ship today:

| Family | Stimulus | Alignment | Detectors |
|--------|----------|-----------|-----------|
| **time-stretch** (percussive) | synthetic drum break | onset-map | transient_sharpness, spectral_centroid, hf_fizz |
| **tonal** (sustained) | synthetic vocal/pad (harmonics + formants + vibrato) | identity | spectral_centroid, hf_fizz, spectral_flux |

```bash
python -m quality_lab.cli run --case tonal --degradation grainy   # fires spectral_flux
python -m quality_lab.cli run --case drum  --degradation smear     # fires transient_sharpness
```

## Deferred detectors (honest status)

- **onset_drift** — prototyped and **deferred**. A body-correlation timing measure
  cannot resolve a few-millisecond drift against a *tonal* hit's quasi-periodic body
  (a ~45 Hz kick cycle-slips; the detector could not distinguish a faithful render
  from a 7 ms drift). It needs a better timing method (e.g. a matched filter on the
  body's noise component, or sub-band envelope timing) before it can be trusted. The
  harness *surfacing* this — rather than shipping a detector that fails its own
  negative control — is the point of the P0a discipline.

See `NOTICE.md` for third-party attribution and the license fence.
