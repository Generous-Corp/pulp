# Native and web projection contract

DSPX-07 adapters advertise a graph feature only after a bounded baked `Processor`
descriptor exists. The installed SDK exposes this decision from
`<pulp/format/projection_capability.hpp>`, so native and browser consumers use
the same rule. CLAP, VST3, LV2, WAM, and WebCLAP use the same capability rule. A
graph-only or unbounded descriptor produces a typed unsupported result and
leaves the source state unchanged. Audio Units are admitted only for the
bounded baked processor path exercised by the AUv2 and AUv3 lifecycle tests.

The projection decision is control-thread metadata. Audio rendering and browser
execution still require the adapter's ordinary descriptor, latency, parameter,
and state round-trip contracts. Dense modulation buffers remain transient and
must never be serialized into descriptor JSON or package metadata.

## Installed consumer proof

Run the downstream validator against an installed Release SDK. It compiles a
temporary consumer with only the SDK include directory, executes the native and
browser positive matrix, and checks typed refusal reasons:

```sh
python3 tools/validation/dspx07_projection_installed_sdk.py \
  --sdk /path/to/pulp-sdk \
  --output artifacts/dspx07-installed-projection.json
```

The receipt is valid only when the public header, `PulpConfig.cmake`, and SDK
provenance are present. A source checkout include path, a missing header, or a
development-only SDK fails closed. Browser module packaging remains a separate
P2 gate until the WAM/WebCLAP helper and runtime closure is installed beside the
SDK rather than resolved from a source tree.

Forge effect consumption uses the existing catalog projection and the same
installed SDK identity. Spectr and GPU-NAM consumer adoption remains deferred to
their external owners. Their receipts require separate authenticated provider,
device/model, immutable artifact identity, runtime fallback, unified-control, and
documentation proof.
