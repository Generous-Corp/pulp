---
name: audio-compare
description: Advisory before/after audio judgment — measure → compare → judge two renders and weigh in on a DSP change with cited evidence (agent-facing, not a gate)
---

Use this when you (an agent) need to **weigh in on a DSP change**: render/capture the audio
before and after, then get a defended judgment — "did this make it worse?" — with cited
evidence, not a bare pass/fail. It wraps the **Audio Quality Lab**'s `compare` surface
(`tools/audio/quality-lab/`, opt-in Python; see the `audio-harness` skill for the full
vocabulary and rationale). Advisory and off-gate: it never fails a build.

**Not this — reach for the gate instead** when you just want an exact/numeric/spectral
before-after *diff* with a pass/fail exit: use `pulp audio validate compare a.wav b.wav
[--mode null|spectral] [--tolerance <dbfs>]` (shipped, deterministic, gate-oriented). Use
`/audio-compare` when you want an *interpreted judgment* an agent can act on.

## One-time setup (opt-in deps)

```bash
pulp tool install audio-quality-lab      # managed venv under ~/.pulp/tools
# — or a plain checkout venv —
cd tools/audio/quality-lab && python3 -m venv .venv && . .venv/bin/activate && pip install -r requirements.txt
```

## Run it

Invoke with two WAVs (`$ARGUMENTS` = `<reference.wav> <candidate.wav> [extra flags]`):

```bash
# Preferred — the shipped CLI verb (delegates to the managed tool; no venv juggling):
pulp audio compare $ARGUMENTS --profile tonal-balance --json /tmp/compare.json
# Equivalent lower-level forms:
#   pulp tool run audio-quality-lab -- compare $ARGUMENTS
#   python -m quality_lab.cli compare $ARGUMENTS
# Agents inside an MCP client can call the pulp_audio_compare tool for the report JSON directly.
```

- **`--reference-role golden`** — declare the reference known-good and the candidate
  *expected unchanged*. This is the ONLY mode that can return `regression_suspected`.
- **default `--reference-role peer`** — a neutral A/B; a changed candidate reads as
  `material_change_detected`, never "regression" (we don't assume which side is right).
- **`--profile`** — the measurement axis. Two today, both global/alignment-free:
  - `tonal-balance` — LTAS spectral-centroid shift; the bad direction is **duller**.
  - `added-hf` — high-frequency (≥8 kHz) energy fraction; the bad direction is **added fizz**.
  - `regression_suspected` also requires the change to be in that axis's bad direction — a
    brighter candidate on `tonal-balance`, or an HF-reduced one on `added-hf`, stays a neutral
    `material_change_detected`. More profiles (distortion, level, pitch) arrive in later slices.
- **`--threshold <t>`** — override the axis's own materiality default (must be in `(0, 1)`).

## Read the verdict

The report (`quality_lab.compare.v1`, JSON at `--json`) has a top-level `verdict` + `summary`
and a `measurements[]` list of typed evidence envelopes:

- `regression_suspected` — golden reference + a materially bad-direction change → investigate/revert.
- `material_change_detected` — a real change on the axis, direction reported (duller/brighter, added/reduced HF).
- `no_material_change_detected` — within threshold; the change didn't move this axis.
- `inconclusive` — couldn't support a verdict (e.g. `not_applicable`: silent/too-short reference).
- `invalid` — couldn't measure (bad WAV, sample-rate mismatch); the CLI exits non-zero ONLY here.

Each measurement carries `status`/`applicable`/`materiality`/`level_match`/`provenance`, so you
can cite *why*: e.g. "LTAS centroid 3200→2750 Hz (duller 14%), RMS matched, no clip/NaN →
regression_suspected." Quote that evidence when you weigh in — don't just report the label.

When the measurement succeeds, the report also carries an off-gate `advisory` block: a
deterministic `null_residual` raw comparator (level-matched sample-domain residual vs the
reference, in `db_rel_reference`; lower = more identical) plus a `corroboration` status. Treat
corroboration as a **materiality cross-check, NOT a trust score** — it only says whether that
raw residual *also* registers a material change. The useful case is disagreement:
`not_corroborated` with `raw_material:true, axis_exceeds:false` means a real difference this axis
can't see (e.g. a pure delay — try another `--profile`). Both raw comparators and corroboration
are `experimental` / `participates_in_verdict:false`; they never change the verdict — cite them as
extra context, never as confidence.

## Diff-integration: judging a DSP change in review (advisory)

When a diff touches a DSP path and you want to say whether it helped or hurt:

1. Render/capture **before** (base) and **after** (branch) to WAVs. Two ways:
   - **Offline, no device (preferred in review):** `pulp audio render --plugin <bundle> --out
     before.wav --duration-ms 1000 …` on each side, or reuse existing golden/captured renders.
   - **Live/hosted plugin:** capture the steady-state window with `pulp run --plugin <bundle>
     --audio-capture-rolling before.wav --headless` (announce the audio + cap the duration + tear
     down per the CLAUDE.md local-dev audio etiquette; this opens the live device).
2. `pulp audio compare before.wav after.wav --reference-role golden` (base is the known-good).
3. Attach the `summary` + cited evidence to your review note. **Advisory only** — it informs
   your judgment; it does not gate the PR, and it requires the opt-in lab deps (never wire it
   into default CI).
