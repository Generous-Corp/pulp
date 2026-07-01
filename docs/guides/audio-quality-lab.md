# Audio Quality Lab

The Audio Quality Lab answers one question automatically: **did a DSP change make a sound
*worse* — and where?** It compares a candidate render against a reference and reports, per
artifact (transient smear, dulling, metallic sizzle, graininess, …), whether the candidate
degraded and the timestamp where it's worst.

**Why it exists.** The hardest audio bugs gate on a human A/B listen — “the stretch sounds
a bit crunchy now” — which peak/RMS/clip checks miss entirely and which no one wants to
re-listen for on every change. The lab turns those judgments into objective, localized,
testable signals so an **agent or CI can hear a regression** and fail a test, instead of a
person catching it three commits later. It is perception-aware: targeted DSP detectors at
the core, with optional perceptual models and an optional review model layered on top.

> **This is a developer / CI tool, not an end-user plugin feature.** It is offline, opt-in,
> and never links into the MIT core or a shipped plugin — the FFT/analysis stays tool-side,
> and Pulp's basic audio tests keep working with zero new dependencies. It's for people
> **tuning Pulp's own DSP** (and the agents doing it) and for **developers building on
> Pulp** who want the same “did this get worse?” guardrail on their own sounds.

## Install (opt-in)

Managed install — provisions an isolated venv under `~/.pulp/tools/`, the same
[`pulp tool`](../reference/extending-pulp.md) lane as `ffmpeg`/`uv`:

```bash
pulp tool install audio-quality-lab          # needs a Pulp source checkout + network (numpy/soundfile)
pulp tool run audio-quality-lab -- run --case drum --degradation smear
```

Or a plain checkout: `cd tools/audio/quality-lab && python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt`, then `python -m quality_lab.cli <args>`.

## Try it in a minute

```bash
# 1. Score a synthetic case (degrade a drum break, see which detectors fire + where)
python -m quality_lab.cli run --case drum  --degradation smear --out-dir out
python -m quality_lab.cli run --case tonal --degradation grainy

# 2. Point it at the REAL Pulp stretch engine (needs a built stretchcli, below)
python -m quality_lab.cli engine --ratio 2.0 --character clean
python -m quality_lab.cli engine --input yourfile.wav --character varispeed   # any WAV

# 3. Regression gate: did an engine change make it worse than the committed baseline?
python -m quality_lab.cli engine-baseline

# 4. Agent-facing before/after judgment over two WAVs (advisory; not a gate)
python -m quality_lab.cli compare before.wav after.wav --profile tonal-balance --json report.json
python -m quality_lab.cli compare golden.wav candidate.wav --reference-role golden
```

`compare` is the agent-facing **measure → compare → judge** surface: it level-matches, runs one
curated measurement (`--profile tonal-balance` → the LTAS spectral-centroid shift), and returns a
typed evidence envelope plus an action-oriented verdict (`regression_suspected` /
`material_change_detected` / `no_material_change_detected` / `inconclusive` / `invalid`). It is
**intent-safe** — `regression_suspected` needs `--reference-role golden` — and **advisory**: it
exits non-zero only when it couldn't measure, never for a judgment. So an agent tuning DSP can
weigh in on a change with cited evidence instead of a bare pass/fail.

`engine` / `engine-baseline` validate the real product DSP, so they need its `stretchcli`
harness built once (`cmake -S . -B build -DPULP_ENABLE_GPU=OFF && cmake --build build
--target stretchcli`); the lab finds it via `PULP_STRETCHCLI` or by walking up from your
checkout. Without it those commands `skip` with an actionable message — nothing else needs
it. Each run flows through pure stages — `generate/load → level-match → align → detect →
report.json` — so loudness never decides an A/B and length/latency differences are aligned
out before any detector runs.

## What it detects (stable)

These detectors are validated and **count toward the verdict and the regression gate**:

| Detector | Catches | Material |
|----------|---------|----------|
| `transient_sharpness` | percussion attack smear (“compressed” drums) | percussive |
| `spectral_centroid` | brightness loss / dulling | any |
| `hf_fizz` | added metallic high-frequency sizzle | any |
| `spectral_flux` | graininess / temporal instability | sustained |
| `hnr` | added noise / roughness (tonal purity loss) | sustained |
| `stereo_width` | stereo-image collapse / phase damage | stereo |

Each fires on its own artifact and stays quiet on the others and on an identity render.
They're validated **non-circularly** — against synthetic degradations, an independent
textbook phase vocoder, *and* the real Pulp stretch engine — which is why they're trusted
to gate. (`stereo_width` operates on `(N,2)` arrays directly; the rest run through the mono
pipeline. Full list + module map: [`README.md`](https://github.com/danielraffel/pulp/blob/main/tools/audio/quality-lab/README.md).)

## Maturity — how a feature earns the right to gate

Every detector (and the optional layers below) carries a **`maturity`**, and that single
field decides whether it can affect a pass/fail:

| State | Counts toward `verdict`? | In the regression gate? | What it's for |
|-------|:---:|:---:|---|
| **`experimental`** | no (advisory) | no | a new, unproven signal — runs and reports under the report's `advisory` block so you can eyeball it while tuning, but it **cannot fail a build** |
| **`beta`** | yes | no | trusted enough to call a verdict, not yet to freeze a baseline against |
| **`stable`** | yes | yes | proven; participates everywhere |

**This is the safety mechanism that lets us add unproven “ears” without risk:** an
`experimental` detector that misfires changes nothing that matters. **It also tells you how
to read a result** — a FIRED line marked `(advisory:experimental)` is a hint to investigate,
not a regression. A feature **graduates** only when it clears the validation bar documented
with it (a calibration sweep, an answer-key agreement score, a false-positive sweep) — never
on vibes. Promotion is a one-line `maturity` change once the evidence is in.

## Experimental & advisory features

Useful but not yet proven — all **off the gate**, all developer-opt-in.

**`onset_drift`** — timing / groove drift (the axis no other detector covers: a hit landing
a few ms early/late while every spectral check reads clean). Runs on the percussive case;
try it via `run --case drum`. *Honest state:* the metric (event-time residual after removing
the common latency) recovers an injected drift to ~0.3 ms in calibration — far better than
the earlier approach, which was deferred for being unreliable — but it's percussive-only and
loses accuracy past a ~12 ms drift (it reports `UNCERTAIN` rather than guess). *Graduates to
beta when:* the real-engine negative control + a false-positive sweep across tempos/seeds
pass.

**Perceptual models** — a coarse, full-reference “is it perceptually worse overall” guard,
complementary to the localized detectors. Each is opt-in via its **own** env-path, never
bundled, and skips independently when absent — so you enable any subset (or all) just by
which env-paths you set, and public CI (none set) skips the whole layer:
[ViSQOL](https://github.com/google/visqol) (`PULP_VISQOL_BIN`, MOS-LQO),
[PEAQ](https://en.wikipedia.org/wiki/PEAQ) (`PULP_PEAQ_BIN`, ITU-R BS.1387 ODG), and
[AQUA-Tk](https://github.com/Ashvala/AQUA-Tk) (`PULP_AQUATK_BIN`, PEAQ-family ODG). GPL
tools stay developer-local. Advisory only. This layer is deliberately **full-reference,
music/general-audio**: speech-intelligibility metrics (PESQ, POLQA) and no-reference
neural speech metrics (DNSMOS, NISQA) are out of scope — they’re band-limited or tuned to
speech and don’t fit the reference-vs-candidate contract on musical material.

**MIR structural oracle (aubio)** — a *separate*, advisory cross-check, not a quality
metric. [aubio](https://github.com/aubio/aubio) (`PULP_AUBIO_BIN`, GPL-3.0,
developer-local) is a feature extractor, not a MOS predictor, so it does not sit beside
the perceptual models above. It gives an **independent** second opinion on onset/timing
structure (surfaced under the report’s `advisory.mir_oracles` block) — useful for
non-circularly validating the experimental `onset_drift` detector. Never a gate, never a
committed baseline.

**Advisory reviewer** — a model reads the report (+ optional clips) and names what sounds
wrong in plain language, catching novel/compound artifacts no fixed detector encodes.
Bring-your-own model: point `PULP_QLAB_REVIEWER_CMD` at any subprocess that reads
`{report, assets}` JSON and returns `{summary, suspected_artifacts, confidence}`; run with
`run --review`. *Honest state:* **never a gate** (a confidently-wrong model can't fail a good
change), no network or audio leaves your machine unless your provider chooses to, and it's
unvalidated until you measure it. *Graduates when:* `reviewer.score_agreement` (precision/
recall vs the synthetic answer key) and a real-audio spot-check clear a bar.

**Autonomous tuning loop** — `quality-lab loop` scores candidates, ranks them, and writes
**label proposals** to `corpus/LABEL_PROPOSALS.json`. It is **proposal-only** — it never
edits the corpus ground truth and never auto-promotes. A **Goodhart guard** refuses any
candidate that games one detector while regressing another (normalized Pareto across a
working + held-out slice); low-confidence wins are held `NEEDS-EAR` for a human listen. The
loop proposes; you decide. *State:* first slice — wiring it to the full engine matrix is the
next step.

## How to trust a verdict

- **Coverage** — a detector reports how many onsets it measured; a `clean` verdict with low
  coverage reads `UNCERTAIN`, never a silent pass.
- **Real-engine baseline** — `engine-baseline` freezes the stable detectors' scalars on the
  actual engine; a future build that deviates is flagged. Experimental/beta detectors are
  held out of it, so they can't cause a false regression.
- **Provenance** — every report records the engine commit, recipe, and determinism context,
  so a render you liked maps back to how it was made.
- **License fence** — copyleft/heavy tools are reached only via an explicit env-path, never
  bundled; the committed corpus stays permissively licensed.

## Relationship to the existing audio harness

This builds on — does not replace — the offline audio-observability harness in
[testing.md](testing.md) (presence / level / THD / response). The Quality Lab adds the
*reference-vs-candidate perceptual artifact* layer for fine-tuning. Module map, full
detector status, and the contributor guide:
[`tools/audio/quality-lab/README.md`](https://github.com/danielraffel/pulp/blob/main/tools/audio/quality-lab/README.md).
