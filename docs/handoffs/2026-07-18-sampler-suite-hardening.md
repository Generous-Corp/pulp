# Sampler suite hardening continuation handoff

Updated: `2026-07-18` (M3 continuation checkpoint)

## Objective and completion gate

Finish gates G1-G8 in the private
[sampler hardening plan](https://github.com/danielraffel/pulp-planning/blob/main/2026-07-17-sampler-suite-hardening.md),
including the audio harness, Audio Quality Lab, exact final-revision Release and
sanitizer matrices, refreshed benchmark evidence, an exact Codex GPT-5.6 Ultra
closure review, current public sampler documentation, and merge on green CI.
Do not call the work complete until every final-revision gate and artifact is
proven.

## Remote checkpoint

- Repository: `https://github.com/danielraffel/pulp.git`
- Branch: `feature/sampler-suite-hardening`
- Immutable current handoff tag: `sampler-suite-hardening-handoff-20260718-r2`
- Rebased base: `df2f302b12d0a2747c2c395f28ca7a8ea4cd2e85`
- Rebased implementation checkpoint before the latest adversarial edits:
  `c1831749160f1ed75703bfb4ab567056517671a0`
- Partial adversarial-correction checkpoint:
  `629153ab63a090146925c1e63acb2e354e836c9b`
- The branch and current tag include this guide as the next commit after that
  correction checkpoint. The older tag
  `sampler-suite-hardening-handoff-20260718` is retained for provenance but is
  superseded and must not be used as the continuation head.

Restore without relying on any prior machine state:

```bash
git clone https://github.com/danielraffel/pulp.git
cd pulp
git fetch origin --prune --tags
git switch -c feature/sampler-suite-hardening \
  --track origin/feature/sampler-suite-hardening
test "$(git rev-parse HEAD)" = \
  "$(git rev-list -n1 sampler-suite-hardening-handoff-20260718-r2)"
git merge-base --is-ancestor origin/main HEAD
git status --short
```

Expected: the tag and branch resolve to the same commit, `origin/main` is an
ancestor, and status is clean.

## Boundaries and commit policy

- Do not modify `core/signal/resampler.hpp` or `test/test_resampler.cpp`.
- Do not stage the `planning` submodule gitlink. Update the private planning
  repository on its own `main` only when final evidence is ready.
- Preserve unrelated worktree changes and primary-checkout dirt.
- Every new sampler or sampler-docs commit must end with this exact paragraph:

```text
Reference-Lineage: cleanroom first-principles; no third-party source consulted
Skill-Update: skip skill=content reason="sampler-mip documents an audio CLI command and does not change content-pack workflows"
Skill-Update: skip skill=kits reason="sampler-mip documents an audio CLI command and does not change kit workflows"
```

## Integrated implementation

The checkpoint contains the sampler hardening implementation and partial
adversarial corrections, rebased onto the recorded `origin/main`:

- decomposed sampler tests, stream service, and PulpSampler runtime files;
- persisted authenticated `.pulpmip` production and CLI/help/docs;
- bounded shared preload/page memory accounting and rollback telemetry;
- starvation, loop/reverse, interpolation, octave-mip, and typed synthetic
  heritage behavior;
- bounded source-identity history with stable slot/member IDs and non-zero
  generation wrap protection;
- selection-generation exhaustion that fails closed;
- portable source-only registered benchmark verification;
- stable sanitizer test contracts without weakening functional assertions;
- immutable private disk snapshots for mapped audio sources, retained original
  identity/access-policy handles, and serialized non-RT ranged-reader state;
- public docs aligned with memory-governor semantics, supported live
  replacement, exact mip-selection gates, resident limits, snapshot behavior,
  public module surfaces, and sampler-mip outputs.

The final checkpoint also passes:

```text
python3 tools/scripts/node_abi_gate.py --base origin/main --mode=report
node_abi_gate: Processor and PluginSlot virtual order is additive-only
```

The latest partial correction commit additionally:

- binds registered benchmark verification to the actual benchmark binary;
- replaces minimum-biased timing summaries with median-per-batch and
  median-p95-epoch summaries;
- routes WAV/null evidence through the stateful production `LoopRenderer`;
- adds a CLI-produced-sidecar to `PulpSampler` runtime acceptance probe;
- expands modulation block partitions to `1`, `17`, `64`, and `257` frames;
- strengthens heritage stage tests with independent numerical oracles.

These changes are a resumable WIP checkpoint, not landing evidence. Only the
WAV renderer target was compiled successfully after the edits. The sidecar
probe, sampler test, heritage test, verifier, and complete changed-target set
still require builds and execution.

## Prior evidence that informed the implementation

These results are real but are not final landing evidence because later code or
the rebase changed the revision:

- Corrected combined sampler/service logic: sampler `4,166 assertions / 143
  cases`; stream service `20,209 assertions / 15 cases`; selection exhaustion
  and generation-focused suites passed.
- Immutable mapped-source fix: controlled old-code same-size mutation and
  truncation failures reproduced; new Release and TSan ranged-reader suites
  each passed `36,119 assertions / 5 cases` with no TSan race.
- Earlier 25-binary ASan matrix passed completely.
- AQL control run passed `599`, skipped `42`; four normative sampler/heritage
  CTests passed; all six advisory axes were applicable and clean; the
  graininess negative control failed as intended.
- Sampler null block-partition positive control nulled to `-inf dBFS`; the
  wrong-ratio negative control failed at `-0.1 dBFS` as intended.
- Sampler-mip shellout previously passed `115 assertions / 2 cases`; its help
  CTest passed.
- The exact initial Ultra review thread was
  `019f7664-2df8-73a1-a998-3946feb40b43`. Its four findings were addressed:
  mutable mapped-source safety, unbounded source identities, generation-zero
  wrap, and machine-specific registered benchmark verification.

## Interrupted final-head work

Three fresh exact-checkpoint lanes were deliberately stopped for this handoff;
none produced landing evidence or source edits:

- Release focused 25-target build: configured with tests/examples on and
  GPU/text off; `-O3 -DNDEBUG` verified; stopped during dependencies at about
  6%; no binaries ran.
- ASan focused 25-target build: configured successfully; stopped during
  dependencies at about 20%; no binaries ran and final flags were not yet
  inspected.
- TSan focused 25-target build: configured successfully; `-O3 -DNDEBUG
  -fsanitize=thread` verified; stopped during dependencies before target
  binaries ran.
- Full default-GPU Release build: configured successfully and stopped at about
  6%; no CTests ran.

Restart those gates from fresh build directories. Partial build percentages are
not evidence.

After the latest correction commit, CMake reconfiguration passed and
`pulp-sampler-render-wav` compiled. A wider changed-target build was stopped
during dependencies at about 92 percent to create this handoff. Earlier
four-configuration baseline work was also stopped and became invalid when the
source changed. None of those partial builds is final evidence.

## Open gates

Use the exact arrays and commands in
`docs/reports/sampler-suite-hardening-plan.md`.

1. **Reconfirm current main.** Fetch with prune. If `origin/main` advanced,
   rebase before gathering evidence, rerun the node ABI gate, and recapture all
   revision-bound artifacts.
2. **Final focused matrices.** On the exact final revision, run all 25 Release,
   ASan, and TSan targets/binaries with the documented flags and halt-on-error
   options. The new mapped-source subprocess/concurrency cases must pass.
3. **Coverage regression.** Run the exact Debug+coverage decode-pool CTest with
   its 30-second timeout.
4. **Full Release and audio harness.** Complete the default-GPU full build,
   full CTest, the explicit audio-harness targets, and the documented
   `audio|golden|render|contract|doctor` CTest regex.
5. **Sampler file/CLI integration.** First build and run the new runtime sidecar
   probe and corrected stateful renderer tests. Re-run mapped audio-file,
   ranged reader, streaming source, full sampler, sampler-mip shellout, and
   sampler-mip help.
   Produce a CLI sidecar and prove the production loader accepts it.
6. **AQL and null evidence.** Re-run the pinned oscillator-renderer pytest,
   four normative sampler/heritage CTests, six applicable advisory JSON axes,
   graininess negative control, and block-partition null positive/wrong-ratio
   negative controls.
7. **Benchmark recapture.** The checked 108-row M5 Max artifact is intentionally
   stale after source and methodology changes. Recapture it on the exact final
   revision, then pass supplied-binary, source-only, and self-test verification. Do not treat
   the current expected source-bundle mismatch as a product failure.
8. **Docs and structural checks.** Run `tools/check-docs.sh`, docs-noise lint,
   `git diff --check`, forbidden-file audit, file-size/hotspot audit, and the
   final thermonuclear maintainability review.
9. **Resolve remaining adversarial findings.** Assess the extreme resident-ratio
   fallback from sinc to Cubic Hermite, add or document the appropriate quality
   diagnostic, verify immutable-source behavior on Windows, and update public
   benchmark claims only after recapture. Fix the plan ordering so the null
   commands cannot reference `build-final-aql` before it is configured.
10. **Final exact Ultra closure.** Run Codex GPT-5.6 with explicit `ultra`
   reasoning, read-only access, `origin/main` as the base, and the exact final
   branch. Record dispositions for every finding and repeat until no actionable
   blocker remains. A portable invocation is `codex exec -m gpt-5.6-sol -c
   model_reasoning_effort='"ultra"' -s read-only -C . review --base
   origin/main`.
11. **Plan, PR, CI, merge.** Update the public report and private plan with
    final hashes/results; push docs; open the PR through Shipyard; require all
    checks green; merge; verify `origin/main`; ask before worktree cleanup.

## First safe continuation step

After restoring the tag and branch, run:

```bash
git fetch origin --prune --tags
git merge-base --is-ancestor origin/main HEAD || {
  echo 'origin/main advanced; rebase before validation' >&2
  exit 1
}
python3 tools/scripts/node_abi_gate.py --base origin/main --mode=report
```

Then start the documented fresh Release, ASan, and TSan 25-target matrices in
parallel on the M3. The M5 may be used only as an additional runner; the
handoff does not depend on its worktrees, build directories, or caches.
