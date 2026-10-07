# DSPX-09 independent validation receipt (proof-only)

- Claim: `89873b97-ddd2-4113-ae43-3d209cd6f276`
- Pulp base: `c4ed69529c9df393d65475ff9790d4eb4067fb38`
- Forge dependency: [PR 236](https://github.com/Generous-Corp/forge/pull/236), commit [`4b3b4bc52dc4ade8a0d774a165131e2f3ee0dd75`](https://github.com/Generous-Corp/forge/commit/4b3b4bc52dc4ade8a0d774a165131e2f3ee0dd75)
- Scope: independent Pulp reference tests and namespaced documentation/exposure only.

## Focused executable proof

Follow-up exact-head proof (PR 9834 is already merged):

```text
tools/ci/governed-build.sh cmake --build build --target pulp-test-group-sampler-host-graph
./build/test/pulp-test-group-sampler-host-graph '[dspx-09][follow-up]'
```

Receipt: 4 follow-up cases, 1062 assertions, exit code 0. The executable
retained-history cases use the existing DSPX-03 staging fixture and prove adopt
plus incompatible identity refusal with unchanged graph topology. Delay impulse,
dense automation ordering/overflow, and the typed cross-host projection
comparator are covered by the new follow-up source.

Command:

```text
pulp build --target pulp-test-group-sampler-host-graph
./build/test/pulp-test-group-sampler-host-graph '[dspx-09]'
```

Expected receipt: 3 cases, 12 assertions, exit code 0. (The finite-output check is accumulated across the render so the receipt remains bounded.) The allpass case uses an
independent sinusoidal unity-magnitude oracle; the retained-history case checks
explicit refusal and bounded state; the automation case checks base/offset
composition and non-finite/out-of-range typed refusals.

## Controls and limits

- Positive controls: finite allpass output and unity magnitude; valid retained
  policy; valid base/offset composition.
- Negative controls: explicit `Refuse` policy cannot become `Adopt`; non-finite
  base/offset and out-of-range effective values return typed refusal values.
- Docs/exposure: `sequencer_exposure_check.py` and docs consistency must pass
  against the namespaced row and guide.
- This receipt does not claim packaged Forge distribution, GPU execution,
  Spectr, GPU-NAM, or a shipped host. Those remain deferred to their owners or
  DSPX-Z4.
