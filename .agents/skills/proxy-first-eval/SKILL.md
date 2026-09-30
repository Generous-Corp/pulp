---
name: proxy-first-eval
description: Judge whether a change made things better with demonstrable proxy measures tied to the mechanism it targets, not wall-clock time. Load before choosing acceptance measures for any before/after or A/B claim — CI and fleet speed-ups, build speed, DSP/audio quality, render fidelity. Each proxy names its data source, detection floor and sample size, and every zero is paired with a control on the same instrument.
---

# Proxy-first evaluation

Load this **before** you pick how to measure a change, not after the numbers
are in. Any claim of the shape "X is now better / faster / closer" is in scope:
a CI routing fix, a fleet capacity change, a build-speed tweak, a DSP rewrite,
an importer or renderer fix.

## The rule

**Pick the proxy from the mechanism the change targets.** Ask: what does this
change physically alter? Measure that, at the point where it happens, in a
form someone else can re-count from a log line, a label or an annotation.

Wall-clock time is almost never that. It mostly tracks load, queue depth,
neighbouring jobs and noise. A change can make things better while wall time
rises (the fleet got busier) or leave them unchanged while wall time falls (a
quiet evening). **Wall time is context, never the verdict** — report it, label
it load-dependent, and do not let it decide.

## Proxies by mechanism

### CI / fleet

| Change targets | Proxy | Source |
|---|---|---|
| Placement (routing a job to the right runner class) | share of jobs that landed on a runner of the intended class | completed job `runner_name` / `runner_group_name` / labels, not the workflow's `runs-on:` |
| Starvation | share of jobs cancelled before any runner was assigned | jobs with `conclusion=cancelled` and empty `runner_name` |
| Queue wait | wait **per job ahead in the queue**, not raw wait | `created_at` → `started_at`, divided by jobs queued ahead at `created_at` |
| Gate cost | required-gate runs (or minutes) per merged PR | `shipyard metrics gate-cost` |
| Merge-queue churn | merge-queue attempts per merged PR | `merge_group` runs / PRs merged in the window |
| Queue ejections | ejections **by cause** (red check, timeout, wedge, neighbour failure) | merge-queue events + the failing check-run's `output.title` |
| Build speed | compile units rebuilt, cache hit rate, blast radius of a header | `tools/scripts/build_speed_scorecard.py report --split <ISO-time>` |
| Test-result reuse: benefit | test-seconds skipped ÷ merge-group test-seconds, per group (ctest per-test durations, never elapsed wall time) | ctest log lines / JUnit `time` per test; `reuse_policy_replay.py score` |
| Test-result reuse: safety | false skips (group tests that FAILED which the policy would have skipped) | replay corpus outcome vs policy verdict; live: sampled re-run failures |
| Test-result reuse: flakes | flake-skips (skipped tests whose fail was retried to pass or exonerated) | attempts > 1 in JUnit; exoneration shadow annotation |
| Receipt supply | receipts issued ÷ eligible PR heads | issuer notice / artifact listing vs full-suite green heads |
| Receipt use | reuse ÷ evaluated groups | `shipyard-receipt-decision/v1` annotations, by verdict and reason |

Normalise every count for volume: a rate per job, per PR or per merge, never a
raw total across windows of different traffic.

A reuse policy (anything that lets a merge group skip tests an earlier run
proved) ships only when `tools/scripts/reuse_policy_replay.py score` reads
**0 false skips** over the history window; its benefit is reported beside that,
never instead of it. Controls on the same corpus: the `none` policy reads 0%
benefit, `whole-receipt` on a tree-identical pair reads 100%, and the
`score --scenarios` fixtures include a synthetic failing record that must read
1 false skip. A policy whose coverage is small is "insufficient sample", not
safe.

### DSP / audio

| Change targets | Proxy | Tool |
|---|---|---|
| Correctness vs reference | null residual **with alignment** (dB) | `assert_null_near`, quality-lab `compare` |
| Aliasing / distortion | tone residual by least-squares projection, THD/THD+N | `tone_residual_db()` prior art, Audio Doctor |
| Perceptual artifacts | detector counts with timestamps (transient smear, dulling, metallic HF, graininess) | `pulp tool run audio-quality-lab -- compare` (`/audio-compare`) |
| Filter shape | magnitude response at named frequencies | `signal::frequency_response`, Audio Doctor |

A listening impression is a pointer to where to measure, not a verdict. See
the `audio-harness` skill for the lanes and the window-floor traps (Hann cannot
see −100 dBc; the default `OversamplerT` kind has ~7 dB alias rejection).

### Render / import fidelity

| Change targets | Proxy | Tool |
|---|---|---|
| Layout | per-node box deltas in px | `layout_parity.py` |
| Material survival | properties present in the envelope | `material_audit.mjs` |
| A named region | per-region score | `diff_against_reference_regions.py` |
| Controls work | driven-control assertions | the `prove-before-showing` skill |

A whole-image similarity score is position-blind triage, not a fidelity
verdict. Read each tool's **Cannot see** line in the CLAUDE.md tool registry
before quoting its number.

## Demonstrable means three numbers per proxy

1. **Data source** — the exact log line, label, annotation or API field, so a
   reviewer can re-count it.
2. **Detection floor** — the smallest effect this instrument can see. Prove it
   with a negative control (run it on a case with the defect removed and show
   the reading collapses), don't derive it.
3. **Sample size** — n per side. Small n (a handful of runs, one merge window,
   one render) is **"insufficient sample"**, not a verdict in either direction.

## Controls and instrument traps

- **Pair every zero with a control** on the same instrument and target that
  must return non-zero. If the control is also zero, the instrument is broken;
  report nothing. Compare the control's *count* to what you expect, not just
  "non-zero".
- **Identical results across different filters means the filter is ignored.**
  Example: `actions/runs?workflow_id=...` silently ignores the parameter and
  returns every workflow's runs; use `actions/workflows/<file>/runs`.
- **Do not grep whole job logs** for a marker. Logs echo the step's own script,
  so the pattern matches its own source. Count annotations, `##[notice]` /
  `##[error]` lines, or check-run `output` instead.
- **`actions/jobs/<id>` handed a check-run id returns a coherent, wrong job.**
  Use `check-runs/<id>` for the merge gate's own record.
- **Watch the failure shape, not only success.** A job queued with no runner
  ever assigned, a merge-queue entry with no `merge_group` run, a test that
  SKIPs — all read as "nothing bad happened" to a success-only query.
- **Confirm the before and after measured the same thing**: same workflow, same
  job name, same stimulus, same canvas size, same build type (Release).

## Checklist

- [ ] Named the mechanism the change targets, in one sentence.
- [ ] Chose a proxy measured at that mechanism, normalised per job / PR / merge.
- [ ] Wrote down the data source a reviewer can re-count.
- [ ] Stated the detection floor, proven by a negative control.
- [ ] Ran a positive control for every zero.
- [ ] Recorded n per side; declared "insufficient sample" if it is small.
- [ ] Checked the failure shape (no runner, no run, SKIP), not only success.
- [ ] Reported wall time as load-dependent context only.

## Report template

```
Mechanism:   <what the change physically alters>
Proxy:       <measure, normalised>            before -> after
Source:      <log line / label / API field / tool invocation>
Floor:       <smallest detectable effect; negative control used>
n:           <before n> / <after n>   (insufficient sample if < ...)
Controls:    <positive control for each zero, with its count>
Verdict:     better | worse | no change | insufficient sample
Context:     wall time <before -> after>, load-dependent, not the verdict
```

## Tools

- `shipyard metrics gate-cost` — gate minutes per merged PR, batch fullness,
  receipt reuse. `shipyard metrics compare` for before/after windows; treat
  its timing columns as context.
- `tools/scripts/build_speed_scorecard.py report --split <ISO-time>` — build
  proxies split at the change.
- `tools/scripts/reuse_policy_replay.py collect|score` — replays a
  test-result reuse policy over merge-queue history: benefit, false skips,
  flake-skips, coverage, and the named incident scenarios.
- `audio-harness` skill (C++ lane, gating) and quality-lab / `/audio-compare`
  (advisory A/B with timestamped detectors).
- Visual-compare tools in the CLAUDE.md tool registry, each with its
  **Cannot see** caveat.
- `trace-analysis` skill when the question is "why is this slow" inside one
  process; a trace gives wall and CPU time per slice, which is a mechanism
  measure, unlike end-to-end wall time.
