# G-5 bounded trigger runtime: installed-SDK consumer receipt

State: **PARTIAL**. This receipt proves that the bounded typed trigger runtime
in the exact `origin/main` source head can be consumed and executed from a
freshly installed SDK by a project outside the Pulp checkout. It does not
promote the full G-5 event plane or realtime qualification.

## Provenance

- Source head: `1e591dfe1de462dbca5b39f31ebf2be5c3082ae1` (`origin/main` at
  receipt time).
- Release build: `pulp build` completed successfully. The configured cache
  reports `CMAKE_BUILD_TYPE:STRING=Release`.
- Installed prefix: `/Volumes/Workshop/Code/agent-worktrees/pulp-g5-installed-consumer-receipt-20261104/receipt-20261104-201512/prefix`.
- Prefix manifest: 3,637 files, 599,604,009 bytes, manifest SHA-256
  `a39d6dfdf0fc30c2f2a1e5badc85ffda8623c6379ca7280642b2ee17a466ae34`.
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
`/Volumes/Workshop/Code/agent-worktrees/pulp-g5-trigger-runtime-consumer-external-20261104`;
its source-tree manifest SHA-256 is
`aa7963df5a9a7ee123fe71584c11e7c6d1da8e727f629334620d46c5487edebe`.
The copied `main.cpp` and `CMakeLists.txt` content hashes are respectively
`e49ea1e7be3f39b19a7a41e0ef25de7c531315d48e5192cb460b5bbdf05a9bbc` and
`59ac5e976e0198b04ebec72f55d57e3ee749878af95d7f0388de7abeb1eab48b`.

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
`1/1` passed in `0.15 sec`, and direct execution printed:

```text
PASS: typed onset, ties, envelope, modulation and lowpass audio
```

The consumer binary SHA-256 was
`4f46f2d6765ac21489d170e5de93ca2e37f0d9a13bfe767ed0cb074bc40e0a9b`.

## Planted-negative control

An independent copy changed one expected output sample from `0.125f` to
`0.25f`; no Pulp source or installed SDK file changed. The negative consumer
configured and built successfully against the same prefix, then exited `1`
and printed the measured samples, beginning:

```text
0 0.125 0.125 0.09375 0.171875 0.148438 0.105469 0.0683594
```

The negative copied source-tree manifest SHA-256 was
`7f22a271f1df917d3575b956c7308cb31ddc0db61f15161b4625441b7fb597dd`, and its
binary SHA-256 was
`60e1e87a5ab8cbc42a49e7cce6908aa61c484bb91d28a8f4b0d2958ad6ca586f`.
This is a break-confirming output oracle, not a mutation test of the Pulp
runtime implementation.

## Boundary retained

The bounded runtime receipt covers framework onset production, envelope,
modulation, low-pass processing, event trace ordering, reset, and external
SDK linkage. Full G-5 remains open for graph event queue semantics (including
tie, late, overflow, and queued cross-block behavior), host timestamp
transport, block-size/partition invariance, plugin/MIDI parity, host
scheduling, and allocation-free hard-realtime measurements. No product-facing
authoring/control surface or realtime qualification is implied.
