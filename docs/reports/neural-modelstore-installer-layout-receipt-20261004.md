# Neural ModelStore installer-layout receipt

Date: 2026-10-04
Worktree: `/Users/danielraffel/Code/pulp-modelstore-product-gate`

## Scope

The private GPU-audio manifest seam now has an installer handoff,
`write_neural_model_install_manifest`. A neural installer supplies the existing
ModelStore install metadata path, a complete tensor layout, and the execution
manifest. The handoff verifies every installed path's byte count and SHA-256,
requires the layout to describe the primary artifact, and then publishes the
versioned sidecar by atomic rename. The sidecar persists each tensor asset's
role, path, size, and digest so a new process can reload the same layout.

The generic `pulp::runtime::install_model` path remains intentionally generic.
It downloads and records ModelStore catalog assets, but it does not have the
private execution fields needed to construct a neural manifest (architecture,
model version, runtime, sample-rate compatibility, retained-state schema, or
redistribution policy). It therefore cannot emit a neural sidecar without
fabricating metadata or widening the public runtime ABI. A neural product
installer must call the private handoff after `install_model` succeeds and
supply those fields explicitly; an installed model with no complete handoff
remains ineligible for this neural admission path.

## Evidence

Focused Release build and tests:

```text
pulp build --target pulp-test-group-core-gpu-audio-private
build/test/pulp-test-group-core-gpu-audio-private '[gpu_audio][neural]'
All tests passed (77 assertions in 12 test cases)
```

The focused cases include missing-layout rejection, persisted weights/state
roles and paths, same-size and truncated-byte tampering, and a Unix/macOS
fresh-process reload that verifies the persisted layout before `NeuralProcessor`
prepare/publish. This is control-plane admission evidence; it does not claim a
specific model architecture, provider execution, GPU execution, or product
performance.
