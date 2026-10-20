# Compact SSM CPU contract receipt

Date: 2026-10-17
Baseline: `0d802cb69fd42bb3497219964eae127133272b18` (`origin/main`)
Owner: Pulp `core/gpu_audio` private streaming seam; no Vellum or provider owner
Scope: G7 contract-only, synthetic CPU fixture

This packet adds `compact_ssm_cpu_adapter.hpp`, a private diagonal state-space
adapter with one scalar input/output, one layer, and a bounded two-element state
fixture. Preparation validates immutable artifact ID, SHA-256 identity, license
identity, tensor schema/version, state byte count, host shape, and max frames.
The callback mutates only prepared state. Reset zeroes state; reprepare requires
release and a fresh preparation transaction. Alias, oversized, invalid-shape,
and failed-preparation paths fail closed.

The test oracle independently evaluates the recurrence
`state = decay * state + input_gain * x` and compares all 64 output samples at
`1e-6` maximum absolute error, including reset replay. The lifecycle case covers
retry after failed preparation, idempotent release, quiesce, and alias rejection.

| Field | Value |
| --- | --- |
| artifact | `fixture.compact-ssm` (in-memory synthetic; no distributable model) |
| artifact hash | `sha256:fixture-weights` (test identity only) |
| license | `MIT` fixture declaration; no external weights |
| tensor/state schema | `compact-ssm-diagonal-v1`, state bytes `8` |
| model_support | `false` |
| provider_execution | `false` |
| GPU/Dawn/wgpu | not wired or exercised |

This is CPU contract evidence only. It does not establish a licensed model,
serialized tensor compatibility, provider execution, realtime product support,
or a performance claim. Independent serialized artifact vectors and a product
owner receipt remain required before any support claim.

Focused validation: `tools/ci/governed-build.sh cmake --build build --target
pulp-test-compact-ssm-cpu-adapter` followed by the direct binary passed **151
assertions in 2 test cases**, including typed artifact ID/hash mismatch,
invalid-shape, oversized-frame, and planted RT allocation/lock/blocking
negative controls. The test binary SHA-256 is
`175fb9b6e2fe96ec2d27d1f2b73c4ccae64679af87af38bc04377c8e38431651`.
