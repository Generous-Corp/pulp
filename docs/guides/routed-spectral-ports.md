# Routed spectral ports (PARTIAL)

`SignalGraph` can route profile-one spectral frames between independently
connected built-in stages in its existing serial execution plan. This is a
runtime-only first slice; graph persistence, arbitrary native transforms,
parallel spectral execution and anticipation are unsupported.

The installed-SDK example in `examples/g10-routed-consumer` is standalone:

```sh
cmake -S examples/g10-routed-consumer -B build/g10-consumer \
  -DCMAKE_PREFIX_PATH=/absolute/path/to/installed-sdk
tools/ci/governed-build.sh cmake --build build/g10-consumer
ctest --test-dir build/g10-consumer --output-on-failure
```

For an external CMake consumer, build its configured directory through the
repository's governed build wrapper. The example uses only installed public
headers and `Pulp::host`; it does not require the repository's test harness.

Create a `SpectralIdentity` with FFT size N, hop H, indexed channel count and
exact host sample rate. N is a power of two in [256, 16384], H is in [1,N/2],
and channels are in [1,16]. Create named analysis, identity/mask stage and
synthesis nodes, then connect their typed frame handles. The real-mask factory
owns C*(N/2+1) finite gains in [0,1]. There is no callback-registration API.
Audio ports use ordinary audio connections. Frame edges use named typed
connections; numeric audio connections cannot substitute for frame edges.

Each connected component adds N+H samples of latency at synthesis. Dry paths
receive ordinary audio PDC, while spectral payloads receive no audio delay
line. Drain N+H+N-1 zero samples after finite input; nested components compose
these bounds. Each edge owns ceil(max_block/H) frame slots. Maximum prepared
block size is 4096; retained spectral storage is capped at 64 MiB per snapshot.

Prepare with processing stopped. While the spectral controller is active,
direct topology mutation and pinned execution refuse. Use a prepared topology
edit: `prepare()` builds independent state; `commit()` returns `Staged`.
The render callback captures one offer at its boundary, validates eligibility,
then either activates it with one fresh epoch or preserves the active graph.
An explicit reset or non-loop transport seek clears spectral and audio PDC
history. Candidate activation plus reset uses one epoch. Control calls
`finalize_spectral_transition()` after acknowledgment; `Busy` means a retained
reader still owns state. Staging temporarily refuses mailbox mutation with
`TransitionPending`. Pending mailbox data rejects activation without consuming
it. Plugin/custom-node replacement is outside this first transaction subset.

Domain failures publish fixed coherent spectral diagnostics through existing
live DSP telemetry, even with timing disabled. Polling reports the observed
block, drops at publication and a lower bound on age. Invalid scheduled frames
become zero frames. Fatal errors retain the earlier synthesis prefix and zero
only the affected branch from its cutoff; reset is required for reuse.

Coverage status is PARTIAL. The C++ API is executable; notation/catalog authoring,
full callback timing qualification and the complete production-mutation matrix
must be tracked separately in `docs/graph-program-status/G-10-routed.md`.
