# Dawn WaveNet causal-depth evidence receipt (2026-10-07)

Status: **private/default-off evidence only**. This receipt does not promote a
provider, plugin, package, or licensed product artifact.

## Exact inputs

- Provider revision: `f91da75afe31d4d6f47a6da307e1fbabd1b1691a`
- Linked Dawn archive SHA-256: `73727ddf86ffc34eea6fb6392d8d688f44e317ff37b3bb421f8223c8b8815dc9`
- Provider asset SHA-256: `0ebfe03a209ceefe47edfeae70c3cc6c499583b74f35a26140ea55bad7f1e5a9`
- Provider manifest SHA-256: `aef89fe66224aebfb5b976647f7cbc3cebd4eb5013e1d655158c339a08295f82`
- Named model: `test/fixtures/neural/example.nam`
- Model SHA-256: `66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d`
- Model identity: WaveNet A1, 48 kHz, 131 weights, validated by the shared
  `NamTcnArtifact` parser and conversion path.
- Test executable SHA-256: `2d1d49190eea326a20170443c3a00789b03f0d839657a0484ec01a95a38b1434`

The provider identity prelink receipt reported `provider_identity_status=passed`
and bound the executable to the exact Dawn revision and asset above. The
runtime test was run on the authenticated Apple Metal adapter available to this
host.

## Campaign

Command:

```text
build/test/pulp-test-gpu-wavenet-session "[named-causal]"
```

The private test is compiled only with `PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF=ON`.
It loads the named artifact through the validated parser, builds the model-neutral
GPU descriptor and weights from that same parsed object, and compares both:

- a sequential two-slot reference stream; and
- a four-slot batched stream that reuses transport slots.

Both streams ran 100,000 contiguous blocks at 64 frames and were compared with
the independent CPU oracle from the same parsed artifact.

Receipt emitted by the test:

```json
{"schema":"pulp.gpu-audio.wavenet.named-causal.v1","status":"passed","fixture_sha256":"66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d","blocks":100000,"reference_slots":2,"batched_slots":4,"max_cpu_residual":1.84402e-07}
```

The run completed 13,725,006 assertions with no mismatch. This closes the
previous slot/history corruption observed at sequence 2/4 boundaries.

## Scope and remaining gates

This evidence proves causal-state ordering and CPU parity for one exact named
WaveNet artifact on this authenticated provider/device. It does not prove:

- deadline margin, late-result suppression, fallback, or underrun behavior;
- device-loss and reprepare recovery;
- other block sizes, sample rates, channels, or instance counts;
- a licensed redistributable production artifact;
- a named Forge/packaged consumer or plugin path; or
- product acceptance or a performance ranking.

Those dimensions remain explicitly unaccepted until their own provider-bound
receipts exist.
