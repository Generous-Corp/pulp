# Neural model manifest persistence receipt

Date: 2026-10-02  
Work package: ModelStore / neural artifact metadata

## Result

The existing `pulp::runtime::ModelEntry` and ModelStore install record remain
unchanged. Their purpose is catalog/download state: model id, backend,
checkpoint references, asset hashes, and installed paths. They do not own
execution compatibility or retained-state metadata. Adding those fields to the
public record would be an ABI and migration change for every catalog consumer.

The private GPU-audio manifest seam now has a versioned sidecar persistence
primitive in
`core/gpu_audio/src/detail/neural_model_manifest.hpp`:

* `m1.json` maps to `m1.neural.json` beside the existing install metadata.
* The sidecar records architecture, model version, artifact identity/hash,
  license and redistributability, runtime, sample rate, artifact/state sizes,
  and retained-state schema/version.
* Writes validate the manifest, stage JSON to a temporary file, and publish by
  rename. Readers require the schema id/version, reject malformed or incomplete
  metadata, and re-run the shipped/local-use validation policy after reload.
* Unknown fields are ignored for forward-compatible optional additions.
* The helper is control-plane only; no filesystem or JSON operation is valid on
  the realtime callback path.

No field was added to `ModelEntry`, `InstalledModelRecord`, or the public runtime
ModelStore API. A future model installer can opt into the sidecar after it has a
complete execution manifest. Until then, an installed model without a sidecar
must remain ineligible for a neural runtime that requires this metadata.

## Evidence

Focused Release build and tests:

```text
pulp build --target pulp-test-group-core-runtime
build/test/pulp-test-group-core-runtime "neural manifest sidecar*"
All tests passed (31 assertions in 3 test cases)
```

The full neural manifest filter also passes (49 assertions in 9 test cases).
The tests cover sidecar naming, complete round-trip ownership, temporary-file
cleanup, schema-version rejection, range/path validation, and fail-closed reload
of missing required fields. A full ModelStore cold-reload integration is deferred:
the installer currently has no source of architecture/state/runtime metadata,
so fabricating it from `ModelEntry` would weaken the contract.

## Follow-up gate

Before a product lane consumes the sidecar, add an installer integration test
with a named model fixture that supplies the complete manifest, then prove an
install → process restart → reload sequence and hash/redistribution policy
under the same artifact. That gate can be implemented without changing the
existing catalog ABI.
