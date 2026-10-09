# Dawn exact-provider gate receipt (2026-10-07)

## Disposition

**Exact provider prerequisite: PASS. Dawn neural acceptance gate: CLOSED
NO-GO.** This receipt proves that the manifest-bound Dawn provider can be
configured and exercised on the current Apple Silicon host. It does not claim
that the production WaveNet node has useful GPU depth, realtime deadline
margin, or device-loss recovery.

The lane is closed with this disposition rather than left as an unqualified
blocked item. A future acceptance claim would require a separately reviewed
WaveNet-specific probe and campaign; this receipt does not imply that work is
scheduled or accepted.

## Host and immutable provider identity

- Host: Apple M5 Ultra, arm64, macOS 27.0.0, Metal 4.
- Protected source: `origin/main` at `cc5947021ac6643fb3c361a9809d2a388c10e2e9`.
- Skia cache generation: `darwin-arm64-0ebfe03a209ceefe47edfeae70c3cc6c499583b74f35a26140ea55bad7f1e5a9-receipt-v3`.
- Manifest-bound Dawn revision: `f91da75afe31d4d6f47a6da307e1fbabd1b1691a`.
- Dawn archive SHA-256: `73727ddf86ffc34eea6fb6392d8d688f44e317ff37b3bb421f8223c8b8815dc9`.
- Asset SHA-256: `0ebfe03a209ceefe47edfeae70c3cc6c499583b74f35a26140ea55bad7f1e5a9`.
- Manifest SHA-256: `aef89fe66224aebfb5b976647f7cbc3cebd4eb5013e1d655158c339a08295f82`.

The exact configure used `PULP_GPU_AUDIO_EXACT_PROVIDER_PROOF=ON` and passed
the provider identity validation. The governed build emitted configure,
pre-link, and bound identity receipts.

## Executed evidence

```text
python3 test/verify_gpu_dawn_shared_io_provider.py \
  --probe build-dawn-exact/test/pulp-gpu-dawn-shared-io-provider-probe
gpu shared-I/O verifier passed: 21 scenarios plus in-flight watchdog control

build-dawn-exact/test/pulp-gpu-shared-io-private-convolution-probe
status=passed, cases=12, submissions=82, no transfer-counter violations

build-dawn-exact/test/pulp-gpu-shared-io-private-convolution-probe --timestamps
status=passed, gpu_timing_available=true, timestamp_samples=12,
gpu_elapsed_ns=2058667

build-dawn-exact/test/pulp-gpu-shared-io-convolution-session-probe \
  --scenario=prepare-scope-failure
status=passed

build-dawn-exact/test/pulp-gpu-shared-io-convolution-session-probe \
  --scenario=submit-scope-failure
status=passed
```

The provider verifier covers baseline execution, ordered lifecycle, transfer
counter negative controls, queue failures, late completion, device-loss fault
controls, and the watchdog child kill. The private convolution probe records
real Dawn submissions and GPU timestamps with the manifest-pinned archive.

## Why the neural gate remains a no-go

The exact configure exposes no WaveNet-specific provider probe. The available
WaveNet realtime tests use synthetic channels and do not prove provider
identity, high-water depth of at least two, GPU timestamps, ordered delivery,
late retirement, or device-loss recovery. The existing real-provider
convolution-session baseline exits with status 5; its preparation and submit
fault scenarios pass, but that baseline cannot establish the production
WaveNet contract.

Therefore this receipt promotes the provider prerequisite only. It does not
promote Dawn neural support, a realtime guarantee, a performance win, or a
competitive claim.
