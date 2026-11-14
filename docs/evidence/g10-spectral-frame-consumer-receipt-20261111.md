# G-10 installed spectral-frame consumer receipt

Evidence-only receipt for the bounded analyzed-frame metadata consumer. This
does not qualify spectral runtime routing, synthesis, morphing, realtime use, or
the A3 phase.

## Binding

| Item | Value |
| --- | --- |
| Source implementation head | `c3fe4ca885cc5d2ff080b5fe2a87a86c132b9011` |
| Consumer source | `examples/spectral-frame-codec-consumer/main.cpp` |
| Installed header | `build/a3-sdk/include/pulp/signal/spectral_frame_codec.hpp` |
| Installed header SHA-256 | `43e9fbdc2c1acf08d70523589b8cf25d4a4e98ea531898bfc2978aa5eec72a37` |
| Consumer executable | `build/a3-consumer/spectral-frame-codec-consumer` |
| Consumer executable SHA-256 | `e43920271e713be7e183fdaf740da317e9b1a4836baa21b2371f7bd1dfaf2528` |
| Build profile | Release, installed `Pulp::signal` target |

## Controls

The external CTest run passed **3/3**:

1. Positive roundtrip: receipt prints `analyzed frames=2 origins=[-192,-128]`.
2. Planted unknown profile: decoder rejects profile 2 (`WILL_FAIL`).
3. Planted chronology: decoder rejects a mutated frame sequence (`WILL_FAIL`).

The focused producer codec executable also passed **5,440 assertions in 4
cases**. Hashes above describe the local Release SDK staging and executable;
they are receipts for this run, not release artifacts.
