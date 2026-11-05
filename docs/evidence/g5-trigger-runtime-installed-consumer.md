# G-5 bounded trigger runtime: installed-SDK consumer receipt

State: **PARTIAL**. This receipt proves that the bounded typed trigger runtime
in the exact `origin/main` source head can be consumed and executed from a
freshly installed SDK by a project outside the Pulp checkout. It does not
promote the full G-5 event plane or realtime qualification.

## Provenance

- Historical receipt source head: `fc29b7b842dd9890570e88085ec5df8f6f65910f`.
  This was the live `origin/main` used for the install and consumer run. The
  current live `origin/main` is `57104fc19818c5a1939841bbb9ddce722db4b180`;
  the intervening base change is the release version bump only and does not
  touch the trigger runtime or consumer sources. The receipt branch merge head is
  `e0e84dbdbdd7e1e8a8e0396dfe7bf50a8d6ef503`.
- Release build: `pulp build` completed successfully. The configured cache
  reports `CMAKE_BUILD_TYPE:STRING=Release`.
- Installed prefix: `/Volumes/Workshop/Code/agent-worktrees/pulp-g5-installed-consumer-receipt-20261104/g5-installed-prefix-20261105-211142`.
- Prefix manifest: 3,637 files, 599,604,009 bytes, manifest SHA-256
  `12ea0903bd39a4892a969e6e443886bf3c632654653e41ef5c1cb15525c06c06`.
- Installed `trigger_graph.hpp` SHA-256:
  `c553cd4a35d13a5d4cd6822f5ef8bc254c5f1a3613ea1ff269ddfb1bcf40bccb`.
  The source file has the same content hash.
- Installed `signal_graph_runtime.hpp` SHA-256:
  `0febf6e74d7fc7eec33505b9f0c96e9e92e48eb550b9a1a508de42658c4e302d`.
  The source file has the same content hash.
- Installed `custom_node_events.hpp` SHA-256:
  `e01e87078bef719bd88de116aca4259fd465461d56fb59e34a16f5ce1e473676`.
  The source file has the same content hash.

The external positive consumer was copied to
`/Volumes/Workshop/Code/agent-worktrees/pulp-g5-trigger-runtime-consumer-external-20261105`;
its durable source-tree manifest SHA-256 is
`be15201fa92d729b0032af0969fd6b1d4be19b0bafef000d2448deaacf79b1c8`.
The raw command logs and install manifest are checked in under
[`g5-trigger-runtime-installed-consumer-raw/`](g5-trigger-runtime-installed-consumer-raw/).
The checked-in source copies and their manifest are under
`g5-trigger-runtime-installed-consumer-raw/source/` and
`source-manifest.sha256` (`363013513afcc3f5179beaca6ac1c43641c8f6ab937ffa8bbd0703c5a9d51a38`).

## Commands and results

The receipt used a clean external build directory and only the installed SDK
for Pulp headers and targets:

```sh
pulp build
cmake --install build --prefix "$PREFIX" --config Release
cmake -S "$CONSUMER" -B "$CONSUMER/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PREFIX" \
  -DPulp_DIR="$PREFIX/lib/cmake/Pulp"
cmake --build "$CONSUMER/build" --parallel 1
ctest --test-dir "$CONSUMER/build" --output-on-failure
"$CONSUMER/build/pulp-trigger-runtime"
```

The external configure and build completed with exit status 0. CTest reported
`1/1` passed in `0.18 sec`, and direct execution printed:

```text
PASS: typed onset, ties, envelope, modulation and lowpass audio
```

The consumer binary SHA-256 was
`4f46f2d6765ac21489d170e5de93ca2e37f0d9a13bfe767ed0cb074bc40e0a9b`.

## Planted-negative controls

An independent copy changed one expected output sample from `0.125f` to
`0.25f`; no Pulp source or installed SDK file changed. The negative consumer
configured and built successfully against the same prefix, then exited `1`
and printed the measured samples, beginning:

```text
0 0.125 0.125 0.09375 0.171875 0.148438 0.105469 0.0683594
```

The private-header negative uses source-only
`core/host/src/plugin_slot_vst3_internal.hpp`, which is absent from the
installed prefix. The external copy includes
`#include <pulp/host/plugin_slot_vst3_internal.hpp>`; the prefix absence check
passes and the consumer compile fails with exit `1` and
`fatal error: 'pulp/host/plugin_slot_vst3_internal.hpp' file not found`.
Its source SHA-256 is
`a71b07b45745648635633901cef61878a5b05e9a03122fa7b8b94bb3c082b21b`.

Raw evidence hashes are recorded in `metadata.txt`: positive execution log
`709e35172ec9454bc70c34d13774e4df09a384fe7509838aa59b736343e5048b`, private
header negative log
`1db16b6623fa3ec4d09ba2e814b37e89ba96c98b73a463482d40865dc35bf450`, install
log `372ca3e3e8ab4219c2ed5de03db4b8e8b13958d239d3cfcb251822134e8f3620`, and
manifest `12ea0903bd39a4892a969e6e443886bf3c632654653e41ef5c1cb15525c06c06`.
These are durable raw artifacts, not prose-only claims.

The wrong-output negative is also durable: its checked-in source is
`source/wrong-output-negative/main.cpp.txt` (SHA-256
`e526d5da84f8052660c6d6b9dbfacefdfc24a5ce6304c6ec977076c38ce348bb`), and
`wrong-output-negative.log` records configure/build success followed by the
expected nonzero execution and measured samples. Its log SHA-256 is
`91a96d301eb0d60dfdc8fd22711b47b984d0dc2e844a3ee93f43be1b15d8b33a`.

## Boundary retained

The bounded runtime receipt covers framework onset production, envelope,
modulation, low-pass processing, event trace ordering, reset, and external
SDK linkage. Full G-5 remains open for graph event queue semantics (including
tie, late, overflow, and queued cross-block behavior), host timestamp
transport, block-size/partition invariance, plugin/MIDI parity, host
scheduling, and allocation-free hard-realtime measurements. No product-facing
authoring/control surface or realtime qualification is implied.
