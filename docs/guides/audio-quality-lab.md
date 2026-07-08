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

> **How proven is this? (read this first.)** Honest answer: the **core artifact detectors**
> (dulling, fizz, roughness, smeared attacks, stereo collapse, graininess) are validated and
> trusted enough to fail a build. **Everything else is earlier and lightly tested** — the
> perceptual models, the plain-language review model, the timing-drift detector, and the
> auto-tuning loop are all explicitly marked *advisory* or *experimental*, and **none of them
> can fail anything**. So: trust a clean result from the stable detectors; treat anything
> labeled advisory/experimental as a hint to go listen, not a verdict. The
> [Maturity](#maturity) section spells out exactly which
> is which and how something graduates.

## Common plugin-development scenarios

Use the lab when a change is easy to hear in an A/B test but hard to catch with
peak/RMS/clip assertions:

| Scenario | What you run | What it helps catch |
|----------|--------------|---------------------|
| **Retuning tone** — EQ, filters, saturation, amp/NAM tone, reverb damping, compressor color | `compare golden.wav candidate.wav --profile tonal-balance --reference-role golden`; add `added-hf`, `noise-roughness`, or `graininess` for the artifact you care about | duller output, added fizz, rough/noisy harmonic damage, grainy sustain |
| **Protecting attacks** — dynamics, transient designers, phase-vocoder changes, stretch algorithms | `compare before.wav after.wav --profile transient-integrity --reference-role golden` on onset-bearing material | softened or smeared attacks that peak/RMS checks miss |
| **Checking stereo/imaging changes** — wideners, M-S processors, panners, chorus/flanger families | `compare before.wav after.wav --profile stereo-width`; use the regression net for repeated presets | image collapse, phase damage, negative interchannel correlation |
| **Validating time-domain DSP** — fixed-latency processors, varispeed, pitch shift, or pitch-preserving stretch | `compare ... --align latency`, `--align varispeed:R`, `--align pitch:S`, or `--align stretch:R` | real tonal/roughness defects without false alarms from expected time or pitch movement |
| **Getting a coarse second opinion** | set `PULP_VISQOL_BIN`, `PULP_PEAQ_BIN`, `PULP_AQUATK_BIN`, or `PULP_AUBIO_BIN` for local opt-in tools | advisory perceptual or structural cross-checks when the built-in detector says "maybe" |

## What powers the lab

The lab is a small, local orchestrator around Pulp's own detectors plus optional
third-party tools. The stable built-in detectors are the only layer that can gate;
everything license-fenced is opt-in, developer-local, and advisory unless promoted
through the [maturity process](#maturity).

| Layer | Software | How enabled | What it contributes |
|-------|----------|-------------|---------------------|
| Built-in artifact detectors | Pulp Quality Lab Python package | installed with `pulp tool install audio-quality-lab` | localized verdicts for dulling, added HF, roughness, graininess, stereo collapse, and transient smear; stable detectors can participate in gates |
| Product render/engine hooks | `pulp audio render`, `stretchcli` | built from the checkout; `PULP_STRETCHCLI` can point at `stretchcli` | real-plugin and real-engine candidate renders, baselines, and regression-net runs |
| Perceptual models | [ViSQOL](https://github.com/google/visqol), [PEAQ](https://en.wikipedia.org/wiki/PEAQ), [AQUA-Tk](https://github.com/Ashvala/AQUA-Tk) | `PULP_VISQOL_BIN`, `PULP_PEAQ_BIN`, `PULP_AQUATK_BIN` | coarse full-reference "is it perceptually worse overall?" checks; advisory, skipped when absent |
| MIR structural oracle | [aubio](https://github.com/aubio/aubio) | `PULP_AUBIO_BIN` | independent onset/timing feature extraction for checking timing structure; advisory, not a quality metric |
| Review model | any developer-supplied subprocess | `PULP_QLAB_REVIEWER_CMD` | plain-language review of reports and clips; advisory only, never a gate |

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
python -m quality_lab.cli compare golden.wav candidate.wav --profile added-hf --reference-role golden
```

`compare` is the agent-facing **measure → compare → judge** surface: it level-matches, runs one
curated **axis** (selected by `--profile`), and returns a typed evidence envelope plus an
action-oriented verdict (`regression_suspected` / `material_change_detected` /
`no_material_change_detected` / `inconclusive` / `invalid`). Six axes today:

| `--profile` | axis | measures | bad direction (regression vs a golden reference) |
|-------------|------|----------|--------------------------------------------------|
| `tonal-balance` | `tonal_balance` | LTAS spectral-centroid shift (brighter/duller) | **duller** |
| `added-hf` | `added_hf` | band-relative ≥8 kHz fraction ratio (dB) | **added HF fizz** |
| `noise-roughness` | `noise_roughness` | harmonic-to-noise ratio drop (dB) | **rougher / noisier** |
| `graininess` | `graininess` | relative spectral-flux increase | **grainier** |
| `stereo-width` | `stereo_width` | RMS(side)/RMS(mid) width + interchannel correlation | **narrower / collapsed** |
| `transient-integrity` | `transient_integrity` | per-onset attack-smear deficit (onset-aligned) | **softer / smeared attacks** |

`noise-roughness` and `graininess` are meaningful on tonal/sustained material — that is a
caller-declared contract (you pick the profile), surfaced as a standing caveat in the summary
rather than an automatic tonal/percussive classifier. `stereo-width` is the one axis that reads the
**original 2-channel** signal (every other axis mean-downmixes to mono); mono input on either side
is `not_applicable`, and a candidate whose interchannel correlation goes negative is flagged as out
of phase (mono-incompatible) in the summary.

`transient-integrity` is the phase-vocoder / dynamics artifact axis — "did my DSP soften/smear the
attacks?" (invisible to peak/RMS). It detects onsets on both renders, matches and locks each attack,
and compares the high-band attack rise (the *same* primitive the `transient_sharpness` detector
uses). It is **one-directional by design**: a softening is the regression; a *sharper* candidate is
not a transient regression and reads no change. It needs **onset-bearing (percussive) material** —
fewer than 3 matched onsets is `not_applicable` (it never guesses on a sustained tone). It
self-aligns per onset, so `--align` is a no-op for it and the global sample-domain corroboration is
suppressed (a different, non-onset-aligned domain).

Each axis carries its own materiality default (`--threshold` overrides). Adding an axis is one
registry entry (`_AXES` in `compare.py`) — the shared machinery does the level-matching,
applicability, materiality, and intent-safe verdict. `compare` is **intent-safe** —
`regression_suspected` needs `--reference-role golden` AND a change in the axis's bad direction —
and **advisory**: it exits non-zero only when it couldn't measure, never for a judgment. So an
agent tuning DSP can weigh in on a change with cited evidence instead of a bare pass/fail.

### Golden-render regression net (the daily-driver loop)

Once you have `compare`, the highest-value thing to stand up is a **golden-render regression net**.
The **net** is a *safety net*: a batch of before/after checks you run automatically on every DSP
change (the CLI command is literally `regression-net`). You keep a known-good ("golden") render per
plugin/preset, render the candidate after a DSP change, and `compare` across every wired axis — so
each change arrives with a cited, multi-axis "did anything get worse?" verdict already attached.

```bash
# 1. render the candidate from the changed plugin (shipped CLI; any format/backend)
pulp audio render --plugin build/MyEffect.vst3 --preset plate --in dry.wav --out cand/plate.wav

# 2. run the net over a manifest of before/after pairs
python -m quality_lab.cli regression-net --manifest net.json --json results.json
```

`net.json` lists the pairs (paths resolve relative to the manifest, so a suite commits a portable
net):

```json
{
  "reference_role": "golden",
  "profiles": ["tonal-balance", "added-hf", "noise-roughness", "graininess"],
  "pairs": [
    {"name": "plate-reverb", "golden": "golden/plate.wav", "candidate": "cand/plate.wav"},
    {"name": "saturator",    "golden": "golden/sat.wav",   "candidate": "cand/sat.wav"}
  ]
}
```

**Fail policy (the contract):** the *fail* signal keys off axis verdicts only — the net returns
**exit 1** when (and only when) an axis reports `regression_suspected`. A pair that could not be
measured (a missing/corrupt render → `invalid`, or a malformed manifest) is a broken pipeline, not
a judgment, and returns a **distinct exit 2** so a missing render is never greenlit as clean;
**exit 0** means every pair was measured and nothing regressed. The corroboration column is
**informational and never affects the exit code** —
because the modulated family (chorus / phaser / flanger / vibrato / tremolo / ring-mod) is
time-variant, so its phase-sensitive sample-domain residual reads `not_corroborated` forever (a
known, machine-suppressible false alarm — the `uncaptured_material_difference` headline flag carries
`expected_for: ["time_variant_processing"]`). Gating **proper** stays `pulp audio validate compare`;
this net is advisory reporting attached to a change, not a gate by accretion. The runner script for a
specific plugin suite (e.g. `pulp-classic-effects`) lives with the plugins; `quality_lab.regression_net`
is the reusable reference the suite wires its renders into.

**Per-plugin applicability** — read the change class, not the backend (`compare` measures any render
identically, CPU or GPU). "Regression-net use" means whether a golden/candidate
pair is appropriate for `regression-net`, and which profile or alignment mode to
declare when it is not the default timbral case:

| Change under test | Regression-net use | Notes |
|-------------------|--------------------|-------|
| Timbral (EQ / filter / saturation / amp-NAM / reverb tone / comp tone) | ✅ directly valid | the validated sweet spot — all four axes apply |
| Modulated (chorus / phaser / flanger / tremolo / ring-mod) | ✅ valid; corroboration will read `not_corroborated` (expected, informational) | time-variant; the axis verdict is what matters |
| bendr time-stretch — **fixed-ratio** A/B ("did my stretch *algorithm* get worse?") | ✅ valid | same ratio, so the renders are time-aligned |
| tempo-sampler / delay **constant offsets** | ✅ with `--align latency` | trims the constant lag first so the residual doesn't false-alarm on the shift (see below); refuses if it isn't a reliable pure delay |
| bendr time-stretch — **different ratios** ("compare the 1.5× output to the source") | ✅ with `--align stretch:R` | you declare the stretch ratio; the axes measure the warp-invariant qualities directly and graininess/corroboration are warp-normalized. Refuses if the audio isn't actually a uniform stretch of the reference |
| tape-style **varispeed** speed change (pitch follows) | ✅ with `--align varispeed:R` | resamples the candidate back to the reference speed, then the full comparison applies |
| Stereo width / imaging (widener / panner / M-S / collapse) | ✅ via `stereo-width` (opt-in) | add `"stereo-width"` to the manifest's `profiles`; it reads the 2-channel signal and flags a collapse or an out-of-phase candidate. The mono default profiles skip it |
| Attack smear / transient softening (dynamics, phase-vocoder, transient designer) | ✅ via `transient-integrity` (opt-in) | add `"transient-integrity"`; onset-aligned per-attack. Percussive/onset material only — skipped by the mono default (it would be `not_applicable` on sustained material) |

### Time-alignment before measuring (`--align`)

The `--align` flag brings the pair to a common time base before measuring. `none` (the default) is
the alignment-free path; the other modes are opt-in and each is disclosed on the envelope + summary.

#### Constant delay/offset (`--align latency`)

Every axis is global and alignment-free, so a **constant delay/offset** (a delay plugin, a
tempo-sampler that starts a few ms late) doesn't change the tonal verdict — but the phase-sensitive
null-residual *does* see the shift, so it false-alarms `not_corroborated` ("a real difference this
axis can't see") even though nothing tonal changed. `--align latency` fixes that:

```bash
pulp audio compare before.wav delayed_after.wav --reference-role golden --align latency
```

It estimates a single constant lag (normalized cross-correlation of the attack envelopes), trims
both signals to a common time base, and measures the aligned pair — so a pure delay reads
`no_material_change` **and corroborated**, and a real change *hidden behind* a delay (say a dulling)
is measured cleanly. The alignment is disclosed on the measurement envelope
(`alignment: {policy: "fixed-latency-trim", lag_samples, confidence, applied}`) and in the summary.

**It refuses rather than guess:** below a confidence floor the difference isn't a reliable pure
delay (it's a real timbral/structural change, or a time-*stretch*), so it records
`alignment: {policy: "not_aligned", …}` and measures unaligned — a wrong alignment is worse than
none. Default is `--align none` (no behavior change).

#### Declared tape-speed change (`--align varispeed:R`)

A **varispeed** (tape-style) speed change couples pitch and time — the candidate is the reference
resampled by a ratio `R`. Because a resample is *exact* for this class (not an approximation of it),
`--align varispeed:R` undoes it by resampling the candidate back to the reference's time base, and
then the **entire** alignment-free pipeline — including the phase-sensitive sample residual — measures
the pair unchanged (a clean varispeed reads `no_material_change` **and corroborated**).

```bash
pulp audio compare before.wav varispeed_after.wav --reference-role golden --align varispeed:1.5
```

You **declare** the ratio; the tool **verifies** it against the observed duration ratio and **refuses**
(`policy: "not_aligned"`) if the audio doesn't support the declaration — it never resamples to a wrong
length. A defect *hidden behind* a varispeed (added fizz, roughness) still flags after the resample-back
(anti-masking). The one honest limit is physics: a speed-*up* (`R < 1`) bandlimits to a lower Nyquist,
so a source with substantial near-Nyquist energy legitimately reads slightly duller — that is correct
varispeed behavior, not a resampler artifact.

#### Declared pitch-preserving time-stretch (`--align stretch:R`)

A **pitch-preserving time-stretch** (a phase-vocoder / bendr-style algorithm — candidate ≈ R× the
reference duration, same pitch) has no exact inverse to apply, so — unlike varispeed — the axes
measure the **unwarped** pair directly: LTAS centroid, HF fraction, HNR, and stereo width are
time-averages, so they are warp-invariant and measure exactly the algorithm's spectral/roughness
damage. Two axes are warp-**normalized** off the declared ratio: **graininess** measures the
candidate flux at `hop·R` (else a clean stretch reads a false "smoother"), and **corroboration** binds
to a phase-blind LTAS log-spectral distance (`ltas_residual`) because the sample residual is invalid
across a stretch (the null residual is still emitted, marked not-a-corroborator).

```bash
pulp audio compare source.wav stretched_1.5x.wav --reference-role golden --align stretch:1.5
```

The declaration is verified and **refused** on failure: the candidate must be within ±3% of R× the
reference duration (`R ∈ [0.25, 4.0]`), and on onset-bearing material a **single uniform ratio must
actually fit** — each reference onset is matched to the nearest actual candidate onset around its
`ref_t·R` prediction, and if those residuals scatter, the render is non-uniformly warped and reads
`"onset lags inconsistent with a uniform ratio"`. (Sustained material has no onset landmarks to check
and is unaffected by non-uniformity on the time-average axes, so it is accepted.) So a clean stretch
reads `no_material_change` + corroborated, a grainy or fizzy stretch still flags its defect
(anti-masking), and a non-uniform warp declared uniform refuses rather than measure a bad map.

#### Declared pitch shift (`--align pitch:S`)

A **pitch shift** (`S` semitones, duration unchanged) leaves the time base alone, so the axes measure
the pair directly — but a shift moves the whole spectrum, so **tonal-balance** compensates: a perfect
`S`-semitone shift moves the spectral centroid by exactly the pitch ratio, and the axis reports the
candidate's deviation from that expected move (the shifter's added dulling or damage), not the shift
itself. The corroborator compares the candidate against the shift-compensated reference spectrum.

```bash
pulp audio compare source.wav shifted_up3.wav --reference-role golden --align pitch:+3
```

It's verified (`|S| ≤ 24` semitones, duration preserved — a length change means it isn't a pure pitch
shift) and refused otherwise. A clean shift reads `no_material_change`; a shift that also dulls flags
the dulling. **Only `tonal-balance` is valid under `--align pitch`** — the other axes are pitch-variant
(a shift genuinely moves the HF band, the harmonic-to-noise lag, and the attack high band, so they
would false-flag a clean shift), and each declines with `not_applicable` under a pitch alignment.

#### Estimate the stretch ratio (`--align ratio:auto`)

When you don't know the stretch ratio, `--align ratio:auto` estimates it — but only when it can
verify the estimate two independent ways: the duration ratio (candidate vs reference length) and the
slope of the onset times must agree. If they do, it applies the estimated ratio through the same
`stretch:R` path; if they disagree, or the material has too few onsets to check, it **refuses** (a
one-way guess is not trustworthy — declare `stretch:R` yourself in that case).

```bash
pulp audio compare source.wav longer_take.wav --reference-role golden --align ratio:auto
```

It reliably estimates onset-bearing uniform expansions; compressions and non-uniform or ambiguous
material refuse rather than guess.

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

<a id="maturity"></a>

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
loop proposes; you decide. *Honest state:* early and unproven — it runs and writes proposals
today, but it isn't wired across the full engine matrix yet, so treat its output as a starting
point for your own listening, not an answer.

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
