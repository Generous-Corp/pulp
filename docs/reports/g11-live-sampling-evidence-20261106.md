# G-11 live-sampling installed-SDK receipt

Date: 2026-11-06  
Scope: bounded `SignalGraph` sample-region cookbook; no recording or realtime claim

Evidence status: **current-head installed-SDK receipt retained**. The receipt
bundle is committed under
[`docs/evidence/g11-live-sampling-current-head-20261106`](../evidence/g11-live-sampling-current-head-20261106/).

## Exact source and SDK identity

- Pulp source head: `633825f7af3d4c8d0fee7e4ec139588282bdf0a1`
- SDK profile: `forge-dev`, version `1.76.0`, `darwin-arm64`, Release
- SDK provenance: [`sdk-provenance.json`](../evidence/g11-live-sampling-current-head-20261106/sdk-provenance.json)
- SDK provenance binds `source_git_sha` to the exact source head and records a
  clean source tree, compiler, deployment target, dependency pins, and input
  fingerprint.
- The consumer was copied from the installed SDK into an external directory;
  the validator checked that compile and link inputs did not borrow the Pulp
  checkout.

The checkout-side validator was
`tools/validation/sample_region_installed_sdk.py`. It validated the installed
capability catalogs and handoff, compiled the copied consumer with the governed
Release wrapper, ran the consumer, and retained the selected binding proof
summary. The complete retained-file digest list is
[`manifest.sha256`](../evidence/g11-live-sampling-current-head-20261106/manifest.sha256).

## Positive receipt

The installed consumer accepted six render paths at 48 kHz with a 257-frame
maximum block: source/reload/baked plans, each on regular and irregular callback
schedules. The independent scalar all-pass oracle reported a maximum error of
`6.503505523003383e-08` against a `1e-6` tolerance. Reported PDC latency was
zero. The consumer receipt is
[`consumer-receipt.json`](../evidence/g11-live-sampling-current-head-20261106/consumer-receipt.json),
and the validator envelope is
[`receipt.json`](../evidence/g11-live-sampling-current-head-20261106/receipt.json).

Quality Lab's tonal comparison between source and baked regular renders was
`no_material_change_detected`; that result is advisory. The raw consumer,
configure, build, and Quality Lab logs remain beside the receipts.

## Planted controls

The executable refused two changed graph-contract constructors before exposing
parameters; three signed-bake mutations (tampered bytes, untrusted key, and
legacy loader) were refused; and two rendered-sample mutations were caught by
the scalar oracle and the `-180 dBFS` null detector. Two release/reprepare
lifecycle cases also passed. The installed capability proof summary records 50
public binding/aggregate probes for `signal.sample-region` and
`signal.unit-delay`.

## Boundary

This is a bounded installed-SDK graph cookbook receipt. It proves graph
construction, causal sample-region execution, reload, signed bake admission,
partition invariance, and the listed negative controls. It does not prove a
recording device, callback file I/O, sample publication, realtime deadlines,
or a complete sampler product. Keep those capabilities as separate contracts
until their public APIs and receipts exist.
