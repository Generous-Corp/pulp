# Neural Package A ModelStore admission audit

Date: 2026-10-04
Protected base: `origin/main` at `ff83c0e9fd147dc143af7228eac0e3298e84b35f`
Worktree: `/Users/danielraffel/Code/pulp-neural-package-a-modelstore-audit-20261004`

> **Historical audit snapshot.** This report records the package-A audit at the
> protected base above; it is not a current-head status receipt. For current
> coordination and acceptance state, see the [neural audio program status](../status/neural-audio-program-status-20261004.md)
> and the [neural real-time competitive proof plan](neural-competitive-proof-plan-20261003.md).

## Result

**PASS** for the private neural manifest and ModelStore install/reload gates present
on protected `origin/main`. No public `ModelEntry`, `InstalledModelRecord`, or
runtime ABI changes are required by this slice.

## Evidence

The governed Release build completed successfully:

```text
pulp build --target pulp-test-group-core-gpu-audio-private
```

The complete focused filter passed:

```text
build/test/pulp-test-group-core-gpu-audio-private '[gpu_audio][neural][manifest]'
All tests passed (77 assertions in 12 test cases)
```

The gate-specific runs passed:

```text
[gpu_audio][neural][manifest][persistence]  -> 31 assertions, 3 test cases
[gpu_audio][neural][manifest][provenance]   ->  8 assertions, 1 test case
[gpu_audio][installer][manifest][neural]    ->  9 assertions, 1 test case
[cold_reload][gpu_audio][manifest][neural]  -> 11 assertions, 1 test case
```

The direct ModelStore admission case also passed:

```text
ModelStore entry admits only verified neural artifacts
All tests passed (4 assertions in 1 test case)
```

The tests prove that:

* required architecture/version, artifact identity, runtime, sample-rate, state
  schema, license, and redistribution metadata is admitted or rejected
  fail-closed;
* installed tensor assets are checked for both declared byte count and SHA-256,
  including same-size replacement and truncation fail-before plants;
* the private installer persists a complete tensor layout only after those
  checks, without changing the generic ModelStore record;
* a Unix/macOS child process reads the installed ModelStore metadata and private
  sidecar, verifies every asset, then prepares/publishes the model; tampered
  bytes return the expected pre-prepare failure.

## Scope and limits

The implementation under audit is the existing private seam in
`core/gpu_audio/src/detail/neural_model_manifest.hpp`, exercised by
`test/test_neural_model_manifest.cpp` and registered in
`test/cmake/core_runtime_canvas_signal_tests.cmake`. This is control-plane
admission evidence using the deterministic `MicroTcnModel` fixture; it does not
claim a production NAM/TCN architecture, provider/GPU execution, or product
performance. The fresh-process boundary is covered on Unix/macOS where the test
is enabled.

No source or test gap was found in the requested install/reload and manifest
admission gates; this receipt is the only change in this audit branch.
