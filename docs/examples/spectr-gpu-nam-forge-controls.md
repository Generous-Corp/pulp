# Spectr / GPU-NAM Forge controls

This is the plugin-facing contract for exposing a Spectr or GPU-NAM model in a
Forge catalog. It is metadata and a researcher receipt; it does not add a new
runtime or transport path. The complete example is
[`spectr-gpu-nam-forge-controls.json`](spectr-gpu-nam-forge-controls.json),
validated by `tools/scripts/test_spectr_gpu_nam_forge_controls.py`.

## Control semantics

- `node.type_id` is the stable Forge `CustomNodeType` identity. Increment
  `node.version` when the node's ports or parameter meaning changes. Keep model
  selection out of the type ID so saved graphs can change models explicitly.
- `model.model_id` is the installable model key passed to the runtime model
  store (for example `activate_model("audio", model_id)`). It must be stable
  across providers and must not contain a local path. `model.provider` names the
  adapter that can load the checkpoint; it is a capability label, not a secret
  or a device name. Record `checkpoint_sha256` after installation when the
  provider has a reproducible checkpoint.
- `execution.latency_samples` is the prepared, host-visible plugin delay. It
  must equal the GPU node's declared lead multiplied by block size (or the
  measured fixed delay for a provider) and must be reported before playback.
  `sample_rate_hz` and `block_size` make the receipt reproducible.
- `execution.fallback` is `cpu` only when a continuously prepared,
  latency-aligned CPU oracle exists. Set `fallback_primed: true` in that case.
  Use `silence` when no safe CPU equivalent exists; a late or unavailable GPU
  result must then produce bounded silence rather than stale or guessed audio.
- `receipt` is an append-only researcher record. `provider_identity` identifies
  the authenticated backend/device or explicitly says that the path was CPU;
  `observed_path` records what actually delivered audio (`gpu`, `cpu_fallback`,
  or `silence`). `status: pass` requires a reproducible artifact hash and a
  source revision. `inconclusive` is allowed for exploratory runs but must not
  be presented as GPU support.

## Integration sequence

1. Register the versioned `CustomNodeType` in the Forge catalog and export its
   `type_id`; do not fork the CPU runtime or `GpuAudioTransport` for model
   selection.
2. Resolve `model_id` through the model registry/store on the control thread,
   prepare the provider with the declared block shape, and expose the provider
   label in the UI. Persist only the model ID and provider, never a checkpoint
   path or credential.
3. Prepare the GPU node and its fallback before publishing the graph. Report
   the resulting fixed latency to the host and reject a configuration whose
   measured latency differs from the metadata.
4. Capture one bounded researcher receipt per provider/model/device. Include
   the source revision, model/checkpoint hash, provider identity, observed
   delivery path, and artifact hash. A receipt is evidence for that exact
   combination; it does not generalize to another provider or GPU.

The JSON contract intentionally leaves provider implementation private. It is a
versioned metadata and receipt design for researchers and plugin developers;
the `exposure` object deliberately records `status: metadata_only`,
`catalog_registered: false`, and no `named_consumer`. This matches the current
Forge catalog, which has no `spectr.gpu_nam` registration, and the GPU-NAM
provider plan, which does not yet expose a public neural-program adapter.
The example is not a shipped `CustomNodeType` until a named consumer is
registered in the Forge catalog and its installed-SDK/host acceptance proof is
available. Once that consumer exists, it can use the
public GPU audio SDK and Forge catalog APIs described in
[`gpu-audio-sdk.md`](../guides/gpu-audio-sdk.md).
