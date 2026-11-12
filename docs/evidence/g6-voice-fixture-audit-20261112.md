# G6 voice fixture audit — 2026-11-12

Status: **CONDITIONAL HOLD**. The exact-source installed-SDK consumer passes;
graph save/restore and swap while sounding remain **NOT_CHECKED**.

This is a fixture-only audit from a fresh worktree at exact `origin/main`:

- source head: `6b4004acfe7ff18a9fa5e1d81b0acf474076b3ec`
- branch: `codex/g6-voice-fixture-audit-20261112`
- worktree: `/Users/danielraffel/agent-worktrees/pulp-g6-voice-fixture-audit-20261112`
- lineage: active, owner `g6-voice-fixture-audit`

No runtime, public API, queue, scheduler, or graph-owned voice lifecycle was
changed.

## SDK and execution gate

The registered source-tree wrapper is `cmake-g6-voice-bank-sdk-consumer`,
driven by `test/cmake/test_g6_voice_bank_sdk_consumer.cmake`. The separately
configured installed-SDK example `examples/g6-voice-bank-consumer` was built
under governed build lease `pulp-shipyard-local-73300-1794505590` against this
development SDK:

- SDK: `/Users/danielraffel/.pulp/sdk-dev/forge-v1/darwin-arm64/e777deef9dea919aa00ddafdea86e75e32f2a8dc/b5d7d23fc155`
- provenance: `sdk-provenance.json`, SHA-256
  `58d40f39e7352613870194b194045de4494a84df4052365e28e1460f4b780759`;
  `source_git_sha=e777deef9dea919aa00ddafdea86e75e32f2a8dc`,
  `source_git_dirty=false`, `build_type=Release`,
  `distribution_eligible=false`. That source is this PR's fixture-only commit,
  descended from the audit base `6b4004acfe7ff18a9fa5e1d81b0acf474076b3ec`.
- consumer: `build-g6-voice-exact-20261112/pulp-g6-voice-bank-consumer`,
  SHA-256 `bf395c349309eb63fb2663967ad8df943f53624c23ff79e3b5019c8be3504b49`;
  its CMake cache records `Pulp_DIR` under that SDK, `CMAKE_BUILD_TYPE=Release`,
  and `PULP_SDK_SOURCE_GIT_SHA=e777deef9dea919aa00ddafdea86e75e32f2a8dc`.

Direct execution printed `g6-voice-bank-public-consumer PASS
partitions=1,32,64,127,256 samples=800 sample3=-0.353553 sample6=-0.314663
sample10=0`. The installed consumer's CTest
`g6-voice-bank-public-consumer` passed **1/1**; its transcript is
`build-g6-voice-exact-20261112/Testing/Temporary/LastTest.log` (SHA-256
`234c384f3ddc112a4e5ac5b05c3b6a3f9a7cb39f0cc9f9305b728e8caeb44f45`).
The source-tree wrapper CTest and a planted mutation control remain
**NOT_CHECKED** in this run.

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

No fixture expansion was made. P2 remains under CONDITIONAL HOLD pending the
graph lifecycle and swap oracle.

## Reproduction

From this worktree with the SDK above available:

```text
./build-g6-voice-exact-20261112/pulp-g6-voice-bank-consumer
ctest --test-dir build-g6-voice-exact-20261112 --output-on-failure -R '^g6-voice-bank-public-consumer$'
```

The installed consumer pass does not establish graph save/restore or swap while
sounding. Those lanes remain **NOT_CHECKED**, and this audit remains under
**CONDITIONAL HOLD**.
