# Sample-region allpass

This mono `Processor` owns an editable `SignalGraph` containing the recurrence
`y[n] = a*x[n] + x[n-1] - a*y[n-1]`. Explicit one-sample delays make the
feedback causal. The coefficient uses ordinary host-owned `StateStore`
parameter 2901, range -0.99 to 0.99, default 0.5, with no smoothing.
The two semantic delays report zero plugin compensation latency.

`allpass_graph.cpp` constructs a disposable topology candidate with the explicit
built-in registrar. `allpass_processor.cpp` freezes the parameter manifest before
exposing the Processor, binds the adapter's store, and prepares/publishes the
candidate. Call `ready()` and inspect `error()` after preparation. Unsupported
channel layouts and invalid preparation produce silence and an explicit error.
Host reset requests reach the graph through the context-bearing process API.
A fresh successful prepare starts a fresh delay bank.

Use `graph()` to inspect or propose prepared topology transactions. The graph,
frozen contract, and parameter binding have stable lifetimes; edits must preserve
the promoted manifest. Construct a new Processor with saved graph JSON to reload
before host exposure. Do not deserialize directly into a running graph.
Parameter persistence and gestures use the normal Processor state store.

## Installed SDK consumer

Copy this complete directory from `share/pulp/examples/sample-region-allpass`
to a directory outside the Pulp source checkout, then run:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/sdk
cmake --build build --target sample-region-allpass-consumer --parallel 2
./build/sample-region-allpass-consumer ./artifacts
```

The consumer uses only exported SDK targets and installed public headers.
It registers the built-in types, constructs and reloads the graph, signs and
loads a baked artifact with a test-only key, compares structural manifests,
and renders regular and irregular callback schedules against an independent
scalar oracle. It writes graph, bake, audio and measurement artifacts for
review. The deterministic example key is not a product signing identity.
The validation driver additionally archives Quality Lab evidence and hashes.

The sample-region vocabulary is experimental. Native packages, browser targets
and broker-enabled control compositions use this same Processor rather than
reimplementing its DSP.
