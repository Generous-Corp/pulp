# Sampler suite hardening landing handoff

Updated: `2026-07-18` (local landing validation complete)

## Objective and current state

Finish gates G1-G8 in the private
[sampler hardening plan](https://github.com/danielraffel/pulp-planning/blob/main/2026-07-17-sampler-suite-hardening.md),
then land the branch on green CI. Implementation and local executable evidence
are complete. The remaining work is private-plan closeout, PR publication,
required CI, and merge verification; the exact Ultra closure review is repeated
against the final publication commit as the last local gate.

## Exact revisions

- Branch: `resume/sampler-suite-hardening-20260718`
- Rebased landing base: `832f4eb85acfff94f92d1a3b143e2f57762cf204`
- Final runtime source revision: `f08669e06aa05359efea494313b21f0ce249f5b3`
- Benchmark artifact: refreshed after the final rebase in the closure evidence
  commit.

The checked benchmark names `f08669e06aa05359efea494313b21f0ce249f5b3`
as `source_base_revision`. Its source bundle and benchmark binary hashes bind
the refreshed post-rebase capture.

## Boundaries and commit policy

- Do not modify `core/signal/resampler.hpp` or `test/test_resampler.cpp`.
- Do not stage the root repository's `planning` gitlink. Update the private
  planning repository on its own `main`.
- Preserve unrelated primary-checkout changes.
- Every new sampler or sampler-docs commit must end with:

```text
Reference-Lineage: cleanroom first-principles; no third-party source consulted
Skill-Update: skip skill=content reason="sampler-mip documents an audio CLI command and does not change content-pack workflows"
Skill-Update: skip skill=kits reason="sampler-mip documents an audio CLI command and does not change kit workflows"
```

## Integrated implementation

The branch contains reusable sampler audio primitives and a production-shaped
`PulpSampler` integration covering:

- shared bounded page streaming with two decode workers, generation-safe
  retirement, cancellation, and conservative shared-worker throughput
  admission;
- immutable ranged WAV and uncompressed AIFF snapshots, strict codec
  admission, and persisted authenticated `.pulpmip` sidecars;
- forward, reverse, ping-pong, crossfade, starvation, lookahead, and
  independently positioned eight-voice playback;
- prepared hold, nearest, linear, cubic, and ratio-tracking sinc interpolation
  with resident and persisted mip selection;
- typed synthetic heritage profiles, deterministic state continuation,
  host-rate reset semantics, latency reporting, and clock-aware stream-domain
  rebinding;
- checked prepare/load results, shared memory accounting, and coherent
  diagnostics.

## Final local evidence

- Focused Release: all 25 binaries passed.
- ASan: all 25 binaries passed with
  `detect_leaks=0:halt_on_error=1`.
- TSan: all 25 binaries passed with `halt_on_error=1`.
- Debug+coverage: bounded cross-source decode-pool concurrency CTest passed.
- Sampler integration: 4,255 assertions in 145 cases.
- Post-rebase default-GPU Release: full build passed. The aggregate CTest run
  passed 14,551 of 14,552 tests; the sole failure exposed a test predicate
  racing cache teardown against ownership-slot teardown. After the predicate
  was fixed, the case passed 100 consecutive processes and the full sampler
  binary passed all 4,255 assertions in 145 cases.
- Audio harness: 789 of 789 tests passed.
- Audio Quality Lab: 599 passed, 42 skipped; all four configured
  sampler/heritage gates passed.
- Advisory AQL: tonal balance, added HF, noise roughness, graininess, stereo
  width, and transient integrity were applicable and clean; the graininess
  negative control reported `regression_suspected`.
- Sampler null: block sizes 1 and 257 nulled at `-inf dBFS`; the 1.25x
  wrong-ratio control failed at `-0.1 dBFS`.
- Sampler-mip CLI: 119 assertions in two cases and the help CTest passed.
- Benchmark: all 108 rows passed their budgets; supplied-binary, source-only,
  and self-test verification passed.

## Adversarial review history

Three exact Ultra review cycles produced actionable findings, all fixed with
regressions before the evidence above:

1. decode-pool lost wakeups, concurrent same-size snapshot mutation, duplicate
   ranged snapshots, and equal-priority scheduler displacement;
2. heritage stream rebind budget loss and insufficient stream-registration
   proof;
3. source-local accounting of a shared serial decode-worker capacity.

The complete branch then passed an exact Ultra closure review with no actionable
correctness regressions. Landing repeats the same command against the final
post-rebase publication commit:

```bash
codex exec -m gpt-5.6-sol -c model_reasoning_effort='"ultra"' \
  -s read-only -C . review --base origin/main
```

After that gate, update the private planning repository, push this branch, open
the PR through Shipyard, require all checks green, merge the pinned head, and
verify that `origin/main` contains it. If `origin/main` advanced before push,
rebase first and repeat every revision-bound gate affected by the rebase.
