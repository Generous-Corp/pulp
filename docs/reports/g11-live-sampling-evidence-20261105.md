# G-11 live-sampling installed-SDK receipt

Date: 2026-11-05  
Scope: bounded `SignalGraph` sample-region cookbook; no recording or realtime claim

Evidence status: **historical prose-only run note**. The receipt, executable,
copied consumer, SDK prefix, and raw logs from this run were not committed.
The paths and SHA-256 values below identify ephemeral files from that run; they
cannot be independently verified from this repository and do not establish a
current `origin/main` result.

The copied installed example at
`share/pulp/examples/sample-region-allpass` was configured and built outside
the checkout against the official Release SDK `1.6.0` (`darwin-arm64`). The SDK
provenance records clean source `c02bf6f190eb3a8d7e3c9883aa816f2f5a09be38`
(`v1.6.0`) at the time of that historical run. Configuration used
`CMAKE_BUILD_TYPE=Release`, Ninja, and the exported `Pulp_DIR`; the build used
the Pulp checkout's `tools/ci/governed-build.sh` wrapper.

Historical positive run:

```text
Installed allpass consumer passed: six renders, scalar/null/Doctor gates, signed artifact round trip
```

The ephemeral receipt (`pulp.sample-region-installed-consumer.v1`) reported six render
paths, sample rate 48 kHz, maximum block 257, zero reported latency, and a
maximum scalar-oracle error of `6.5035055230033834e-08` (tolerance `1e-6`).
The historical executable SHA-256 was
`eb0a05e69625f55eb111aa651d7e60ed02284634b9ac637342b7b2528972d391` and the
historical receipt SHA-256 was
`c51939e7842cb093758f0071f67f83f14df7fc87d6a49718d7c6b13ec38d3bc6`.

Historical planted controls in the unchanged consumer passed: two changed graph-contract
constructors, three signed-bake trust/version mutations, and two rendered
sample mutations (scalar oracle plus `-180 dBFS` null detector). These controls
are evidence that graph identity, artifact trust/version, and rendered samples
are checked; they do not qualify recording publication, callback I/O, realtime
deadlines, or a complete sampler product.
