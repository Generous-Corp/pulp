# G-11 live-sampling cookbook

This recipe shows a bounded live-sampling graph workflow through the installed
SDK. It uses the shipped `sample-region-allpass` example as a template for an
editable `SignalGraph` whose scalar kernels run inside the canonical graph
executor. The region contains explicit input, parameter, arithmetic, and output
boundaries plus `UnitDelay` nodes for causal feedback. The Processor owns the
ordinary `StateStore` parameter contract; the callback only executes prepared
state.

## Run the installed consumer

Use an SDK prefix that contains `share/pulp/examples/sample-region-allpass` and
build the copied example outside both the SDK and the Pulp checkout. Set
`PULP_SRC` to a readable Pulp checkout only because the governed-build wrapper
lives there; the consumer source and build remain external:

```sh
PULP_SRC=/path/to/pulp
SDK=/path/to/pulp-sdk
CONSUMER=/path/to/sample-region-allpass
cp -R "$SDK/share/pulp/examples/sample-region-allpass" "$CONSUMER"
cd "$CONSUMER"
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPulp_DIR="$SDK/lib/cmake/Pulp"
bash "$PULP_SRC/tools/ci/governed-build.sh" cmake --build build \
  --config Release --target sample-region-allpass-consumer
mkdir -p artifacts
./build/sample-region-allpass-consumer artifacts
```

The consumer is compiled from the copied installed example and links only
exported SDK targets. Its positive path constructs the graph, prepares it at
48 kHz with a 257-frame maximum block, renders regular and irregular callback
schedules, reloads the serialized graph, signs and reloads a baked plan, and
compares every output with an independent scalar all-pass recurrence. The
receipt is generated at `artifacts/consumer-receipt.json`; it records six
renders, the `1e-6` oracle tolerance, the maximum oracle error, and the zero
reported PDC. The command above creates this runtime receipt locally; this
repository does not commit that output.

The same executable carries production-negative controls. It refuses renamed
or empty graph contracts before publishing parameters; rejects tampered,
untrusted, and legacy-version baked artifacts; and rejects a planted output
sample error with both the scalar oracle and the `-180 dBFS` null detector.
These controls prove that the positive result is sensitive to graph identity,
artifact trust, loader version, and rendered samples rather than merely to
process completion.

## Scope

This is a bounded installed-SDK graph cookbook. It does not provide a recording
device, callback file I/O, sample publication protocol, realtime timing result,
or full sampler product. Keep those capabilities as separate contracts until
their own public APIs and receipts exist. The lower-level declarations and
causality rules are in [sample-granular graphs](sample-granular-graphs.md); the
template sources and consumer README are installed under
`share/pulp/examples/sample-region-allpass`.
