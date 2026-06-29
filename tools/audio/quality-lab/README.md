# Audio Quality Lab

**Tell, automatically, whether a DSP change made a sound *worse* — and where.**

A perception-aware, offline tool that compares a candidate render against a reference and
reports, per artifact, whether the candidate degraded (transient smear, brightness loss,
metallic high-frequency sizzle, graininess) and the timestamp where it's worst. It exists
so an agent (or a developer) can close the A/B tuning loop without a human listening on
every iteration, and so a regression in those artifacts can fail a test instead of slipping
through.

It is an **additive, opt-in developer/CI tool**: Pulp's basic audio tests keep working with
zero new dependencies, and nothing here is ever linked into the MIT core or a shipped
plugin (FFT/analysis stays tool-side).

## Value at a glance

- **Catches what peak/RMS/clip miss** — targeted detectors for transient smear, dulling,
  metallic HF, and graininess, each localized to a timestamp.
- **Runs on the real engine** — validates the actual Pulp stretch engine, not just a
  reference, and a committed baseline flags when an engine change regresses.
- **Works on real audio** — point it at any WAV; it checks reference-free that a faithful
  stretch preserves the source's spectrum.
- **Trustworthy** — non-circular validation, coverage/confidence on every verdict,
  re-derivable provenance, and a license fence that keeps copyleft/heavy tools out of the
  committed surface.

## Install (opt-in)

Managed install (recommended) — provisions an isolated venv under `~/.pulp/tools/`, no
project or global changes:

```bash
pulp tool install audio-quality-lab
pulp tool run audio-quality-lab -- run --case drum --degradation smear --out-dir out
```

This is the same `pulp tool` lane as `ffmpeg`/`uv`/importers (see
`docs/reference/extending-pulp.md`); `pulp tool list` shows it. The lab is a machine-level
developer/agent tool — never linked into the MIT core or shipped in a plugin. Unlike the
binary-download tools, it pip-installs from **this source tree**, so it requires a Pulp
source checkout (and network access for numpy/soundfile).

Manual venv (equivalent; the lab is a standard Python package):

```bash
cd tools/audio/quality-lab
python3 -m venv .venv && . .venv/bin/activate
pip install -r requirements.txt   # numpy + soundfile (both permissive); add pytest to run tests
```

Optional, for real-engine validation — build the stretch CLI once:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPULP_ENABLE_GPU=OFF
cmake --build build --target stretchcli
```

## Use

```bash
# all detectors on a synthetic case; export listenable reference/candidate clips
python -m quality_lab.cli run --case drum  --degradation smear --out-dir out --out out/report.json
python -m quality_lab.cli run --case tonal --degradation grainy

# validate the REAL Pulp stretch engine (auto-skips if stretchcli isn't built)
python -m quality_lab.cli engine --ratio 2.0 --character clean
python -m quality_lab.cli engine --input yourfile.wav --character varispeed   # any real WAV

# regression gate vs the engine's committed baseline
python -m quality_lab.cli engine-baseline            # check
python -m quality_lab.cli engine-baseline --capture  # re-freeze after an intentional change

# versioned, license-guarded corpus
python -m quality_lab.cli corpus list
python -m quality_lab.cli corpus add --file vocal.wav --name vocal1 \
    --class vocal --license CC0 --expect "graininess on sustained notes"

pytest tests/ -q
```

The pytest suite is intentionally **not** wired into the default `ctest` — the lab's deps
are opt-in and basic testing stays dependency-free.

## Features

### Detectors

| Detector | Catches | Method | Material |
|----------|---------|--------|----------|
| `transient_sharpness` | percussion attack smear (“compressed” drums) | per-onset high-band attack-rise deficit, locally aligned | percussive |
| `spectral_centroid` | brightness loss / dulling | long-term-average-spectrum centroid shift (global) | any |
| `hf_fizz` | added metallic HF sizzle | added >8 kHz energy fraction vs reference (global) | any |
| `spectral_flux` | graininess / temporal instability | mean energy-normalized spectral-flux increase (global) | sustained |
| `hnr` | added noise / roughness (tonal purity loss) | autocorrelation harmonic-to-noise-ratio drop, Boersma-debiased (global) | sustained/tonal |
| `stereo_width` | stereo-image collapse / phase damage | width-ratio (RMS side/mid) drop + inter-channel correlation sign-flip | stereo |
| `onset_drift` *(experimental)* | timing / groove drift | per-onset event-time residual after common-latency removal (sub-hop cross-correlation) | percussive |

Each detector fires only on its own artifact and stays quiet on the others and on an
identity render. `hnr` runs in the tonal and real-audio families (exercise it with
`run --case tonal --degradation noisy`). `stereo_width` is **standalone** — it operates
directly on `(N, 2)` stereo arrays rather than through the mono `run` pipeline (which
downmixes), so call it on stereo reference/candidate when validating stereo-affecting DSP. Each reports **coverage** (how many onsets it actually measured); a
`clean` verdict with low coverage reads `UNCERTAIN`, never a silent pass.

### Case families

The unit of work is a *case* with a family, reference policy, alignment policy, and a set
of detectors — so the same machinery serves more than drums:

| Family | Stimulus | Alignment | Detectors |
|--------|----------|-----------|-----------|
| **percussive** | synthetic drum break | onset-map | transient, centroid, hf_fizz |
| **tonal** | synthetic sustained vocal/pad | identity | centroid, hf_fizz, spectral_flux, hnr |
| **real audio** | any developer-supplied WAV | reference-free (preserve source spectrum) | centroid, hf_fizz, spectral_flux, hnr |

### Real engine validation + regression gate

`engine` runs the detectors on the actual Pulp stretch engine. It catches the engine's
real behavior — the `clean` character smears attacks (transient fires); `varispeed`
additionally dulls brightness (centroid fires). `engine-baseline` freezes the current
engine's detector scalars and flags when a future build deviates — *did this change make
it worse?* — with `transient_sharpness` increasing flagged as **WORSE**.

### Real audio

`engine --input <wav>` runs the real engine on any WAV and checks it **reference-free**: a
faithful time-stretch preserves the source spectrum (timing changes, timbre shouldn't), so
the global spectral detectors compare the engine output's spectrum to the source's.

### Listenable clips + provenance

`run --out-dir` writes the full reference/candidate WAVs plus a ref-vs-candidate clip pair
around each worst region, so “attack softer at 0:02.6” is something you can play. It also
drops a `<wav>.provenance.json` sidecar (engine commit, recipe, content hash) so a render
you liked maps back to how it was made.

### Perceptual models (opt-in, license-fenced)

A full-reference perceptual MOS predictor is a coarse global guard, advisory only — it
won't localize a defect but it flags when a change is perceptually worse overall. These are
reached only across a process boundary via an explicit env-path, never bundled or
auto-downloaded, and they degrade to `skipped` when absent (the default, and always in
public CI):

| Tool | License | Role |
|------|---------|------|
| [ViSQOL](https://github.com/google/visqol) | Apache-2.0 | full-reference perceptual MOS (music + speech) — the recommended starting point |
| [PEAQ](https://en.wikipedia.org/wiki/PEAQ) (ITU-R BS.1387; e.g. GstPEAQ/PeaqB) | GPL | the classic broadcast metric |
| [AQUA-Tk](https://github.com/Ashvala/AQUA-Tk) | GPL-3.0 | bundles PEAQ + embedding distances (FAD, etc.) |

GPL tools stay developer-local. See `NOTICE.md` and the public licensing page for full
attribution.

### Advisory reviewer (opt-in, never a gate)

A reviewer model can read the report (+ optional clips/spectrograms) and name what sounds
wrong in plain language — catching novel/compound artifacts no detector encodes. It is
**advisory only**: it never changes the `verdict` or any gate. Configure a developer-supplied
subprocess provider via `PULP_QLAB_REVIEWER_CMD` (reads `{report, assets}` JSON on stdin,
writes `{summary, suspected_artifacts[], confidence, notes}`); skip-when-absent by default,
no network or audio leaves the machine unless your provider chooses to. Run with
`run --review`; output lands under the report's `advisory.reviewers`. Validate a real
reviewer with `reviewer.score_agreement` (precision/recall vs the synthetic answer key)
before trusting it. Tracked: #5296.

### Corpus

A versioned corpus (`corpus/MANIFEST.json`) of sources by material class, license, and
expected artifact. Generator-backed sources are synthetic and regenerable; file-backed
sources are real audio you supply. `corpus add` rejects any non-permissive license, so the
committed corpus stays license-clean; GPL/proprietary material stays developer-local.

### Regression gate on the real engine

The lab freezes the detector scalars the *current* engine produces (across a ratio x
character matrix) as a committed baseline, and flags when a future engine build deviates:

```bash
python -m quality_lab.cli engine-baseline            # check current engine vs baseline
python -m quality_lab.cli engine-baseline --capture  # re-freeze after an intentional change
```

This is the lab's core promise made concrete: *did this engine change make it worse?*
`transient_sharpness` going up is flagged as **WORSE**; any deviation is surfaced for
review (guard the invariant, not the exact sound). The comparison logic is unit-tested
in CI; the live check needs a built `stretchcli`.

## Credibility — why this isn't circular

A detector validated only against a degradation written by the same author is
self-fulfilling. `transient_sharpness` is therefore also validated against the output of an
**independent textbook phase vocoder** (`reference_pv.py`) and against the **real product
engine** — firing on genuine attack smear in both, and staying clean when a render is
compared to itself.

## Module map

| Module | Role |
|--------|------|
| `quality_lab/schema.py` | case, report envelope, detector result — the stable public shapes |
| `quality_lab/audio_io.py` | WAV load/save + RMS level-match |
| `quality_lab/generate.py` | deterministic synthetic stimuli + controlled degradations |
| `quality_lab/align.py` | onset detection + onset-map (alignment runs before detectors) |
| `quality_lab/dsp.py` | shared primitives (high-band, smoothed envelope, normalized correlation, LTAS) |
| `quality_lab/detectors/` | one detector per module |
| `quality_lab/reference_pv.py` | independent textbook phase vocoder for non-circular validation |
| `quality_lab/engine.py` | adapter to the real stretch engine (`stretchcli`), skip-when-absent |
| `quality_lab/engine_baseline.py` | real-engine regression gate |
| `quality_lab/perceptual.py` | opt-in, license-fenced perceptual-model adapters |
| `quality_lab/reviewer.py` | opt-in advisory LLM/multimodal reviewer (never a gate) |
| `quality_lab/loop.py` | experimental tuning loop: rank candidates, Goodhart guard, label proposals |
| `quality_lab/corpus.py` | versioned, license-guarded corpus |
| `quality_lab/provenance.py` | re-derivable provenance + self-describing sidecars |
| `quality_lab/regions.py` | worst-region clip extraction |
| `quality_lab/pipeline.py` | pure stages: generate/load → level-match → align → detect → report |
| `quality_lab/cli.py` | argument parsing + dispatch |

## Maturity gate (shipping new detectors safely)

Each detector has a `maturity`: `experimental` -> `beta` -> `stable`. An
`experimental` detector runs and reports (under the report's `advisory` block, and
flagged `participates_in_verdict: false`), but its `fired`/`low_coverage` are excluded
from the `verdict` **and** from the `engine_baseline` regression gate. `beta`
participates in the verdict but is held out of the baseline; `stable` participates
everywhere. New detectors ship `experimental` and are promoted only once their
validation clears a bar — so an unproven detector cannot introduce a false regression.

## Deferred detectors & roadmap (honest status)

- **onset_drift** (timing / groove drift) ships **experimental** (advisory — it cannot move
  the verdict or the regression gate; see the maturity gate above). The deferred
  body-correlation approach is replaced by an **event-time residual after common-latency
  removal**: per onset, the candidate's fine event time is found by cross-correlating the
  reference attack against the candidate around the *expected* time (sub-hop precision), the
  median (uniform) latency is removed, and the headline is `max|residual|` in ms. Calibration
  recovers an injected ramp to within ~0.3 ms (far under the 2.67 ms onset hop) for drifts
  inside the ~12 ms search window; larger drifts drop to low-coverage UNCERTAIN rather than
  guess. Promotion to `beta` needs the real-engine negative control + a false-positive sweep
  across tempos/seeds. Tracked in [#5295](https://github.com/danielraffel/pulp/issues/5295).
- **Advisory LLM/multimodal reviewer** ships **opt-in** (`reviewer.py`, `run --review`); see
  “Advisory reviewer” above. Promotion past experimental needs real-audio answer-key
  evidence beyond the synthetic corpus — [#5296](https://github.com/danielraffel/pulp/issues/5296).
- **Autonomous tuning loop** ships its **experimental** first slice (`loop.py`,
  `quality-lab loop`): deterministic candidate ranking, a normalized-Pareto **Goodhart
  guard** (refuses a candidate that games one detector while regressing another; held-out
  slice + NEEDS-EAR), and a **proposal-only** label transaction that writes
  `corpus/LABEL_PROPOSALS.json` and never touches `MANIFEST.json`. It proposes; a human
  decides. Wiring it to the real engine matrix + the full label lifecycle is the next slice —
  [#5297](https://github.com/danielraffel/pulp/issues/5297).

These carry the `post-mvp` + `audio-quality-lab` labels.

See `NOTICE.md` for third-party attribution and the license fence.
