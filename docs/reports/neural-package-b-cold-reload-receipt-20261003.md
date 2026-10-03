# Neural Package B admission and cold-reload receipt

Date: 2026-10-03
Worktree: `codex/neural-package-b-20261003`

## Scope

The private GPU-audio admission seam now verifies installed neural assets
against their recorded byte count and SHA-256 content digest before preparation.
The helper accepts a span of bundle assets, so weights and retained state are
checked in one control-plane admission transaction. It does not change
`pulp::runtime::ModelEntry`, `InstalledModelRecord`, or any public ABI.

The existing versioned sidecar reader remains responsible for validating model
identity, architecture, model version, license, redistribution policy, runtime,
sample rate, and retained-state schema/version. The test then performs a fresh
Unix process reload, verifies the installed metadata and every asset, and only
then prepares and publishes a `NeuralProcessor` instance.

## Evidence

```text
pulp build --target pulp-test-group-core-gpu-audio-private
build/test/pulp-test-group-core-gpu-audio-private '[gpu_audio][neural][manifest]'
All tests passed (57 assertions in 10 test cases)
```

The focused cases include same-size hash tampering, truncated-byte tampering,
sidecar schema/metadata rejection, and install-metadata → fresh-process reload
→ manifest verification → processor prepare/publish. The test also verifies a
two-asset bundle (weights plus state) before allowing preparation.

