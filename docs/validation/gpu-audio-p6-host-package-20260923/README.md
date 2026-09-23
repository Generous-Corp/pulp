# P6 host/package acceptance

This receipt records an installed-SDK consumer smoke against the fresh GPU SDK
used for the Apple-Silicon GPU-audio work. It is package and lifecycle evidence,
not realtime scheduling evidence.

The smoke configured `tools/validation/sdk-smoke` with
`PULP_SDK_SMOKE_REQUIRE_FORMATS=Standalone;CLAP` and
`PULP_SDK_SMOKE_INCLUDE_AUV3=OFF`, using the exact prefix recorded in
`receipt.json`. The Release build used the governed build wrapper. All 17
installed-SDK probes passed, including the host/state/DSL header-and-link probe,
GPU-audio export probe, and public transport lifecycle probe.

The consumer's `pulp_add_plugin` path produced a CLAP bundle with the WebGPU
runtime staged next to the binary. CMake's relocatability check passed and
`clap-validator` reported 15 passed, 0 failed, and 5 skipped tests. Its scan-time
warning (1,614 ms) remains useful packaging/performance context but is not a
failure. The SDK is a development/unmarked prefix, so this receipt does not
cover signing, notarization, or release eligibility.

This prefix detected `Standalone`, `CLAP`, and `LV2`. A configuration requiring
`VST3`, `AU`, or `AUv3` failed closed because the prefix does not export
`Pulp::vst3-sdk` or `Pulp::ausdk`; those formats need a separately built SDK
with the corresponding external SDKs enabled. No realtime deadline conclusion
is drawn here.

The CLAP binary contains an embedded `__FILE__` assertion string naming the SDK
source path. It does not affect loader resolution and did not fail the
relocatability check, but it is recorded as release-artifact path-hygiene
follow-up in the receipt.
