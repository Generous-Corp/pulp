# G6 voice fixture audit — 2026-11-12

Status: **CONDITIONAL HOLD / NOT_CHECKED**.

This is a fixture-only audit from a fresh worktree at exact `origin/main`:

- source head: `6b4004acfe7ff18a9fa5e1d81b0acf474076b3ec`
- branch: `codex/g6-voice-fixture-audit-20261112`
- worktree: `/Users/danielraffel/agent-worktrees/pulp-g6-voice-fixture-audit-20261112`
- lineage: active, owner `g6-voice-fixture-audit`

No runtime, public API, queue, scheduler, or graph-owned voice lifecycle was
changed.

## SDK and execution gate

The registered test is `cmake-g6-voice-bank-sdk-consumer`, driven by
`test/cmake/test_g6_voice_bank_sdk_consumer.cmake`. It stages the SDK from the
parent Release build, then configures and builds the installed consumer. One
governed Release run was **not attempted**: no staged SDK with provenance
matching the exact source head above was available.

Observed SDK state:

- `/Users/danielraffel/agent-worktrees/pulp-p1-sdk-consumer-20261112/build/CMakeCache.txt`
  is `CMAKE_BUILD_TYPE=Release`, but contains no installed `sdk-provenance.json`.
- Installed SDK provenance files under `~/.pulp/sdk` are older source heads;
  none records `source_git_sha=6b4004acfe7ff18a9fa5e1d81b0acf474076b3ec`.
- The historical A1 installed-consumer receipt used source
  `f5eeb16bd24d7226a53cbda781f55cad6fbee72f`, so it is not exact-head evidence
  for this audit. Its receipt is
  `docs/evidence/g6-a1-installed-sdk-receipt-20261112.json` (SHA-256
  `f66ed755720f53c8352825e77b04d6d8e2b8e4711f4d86b043afbbe788156446`).

Therefore the installed-SDK CTest, direct consumer execution, and mutation
proof are **NOT_CHECKED** at `6b4004acfe7f`; no pass or failure is inferred.

## Existing fixture coverage (source inspection)

`examples/g6-voice-bank-consumer/main.cpp` already contains source-level
checks for:

- note-on/note-off and finite release tails;
- sustain and channel-mode handling, including manager/zone expression refusal;
- member MPE pitch, pressure, and timbre expression;
- four steal policies and ordered victim selection;
- slot reuse after release and tracker-capacity refusal;
- stale generation and stale epoch mutation refusal;
- A1 frozen vectors for bounded tail and hard-steal cases.

The fixture does **not** establish graph save/restore or a graph swap while a
voice is sounding. Those remain **NOT_CHECKED**; the A1 replacement vectors are
an offline scripted oracle and do not claim graph lifecycle or swap semantics.

No fixture expansion was made because the exact installed SDK/oracle gate was
unavailable and P2 is under CONDITIONAL HOLD.

## Reproduction once an exact SDK exists

From this worktree, after a governed Release build has staged provenance with
`source_git_sha=6b4004acfe7ff18a9fa5e1d81b0acf474076b3ec` and
`source_git_dirty=false`:

```text
ctest --test-dir build -V -R '^cmake-g6-voice-bank-sdk-consumer$'
```

The run must retain the staged provenance, CTest transcript, consumer output,
and (if available) an independent A1 tail/swap receipt. Until then this audit
remains CONDITIONAL HOLD / NOT_CHECKED.
