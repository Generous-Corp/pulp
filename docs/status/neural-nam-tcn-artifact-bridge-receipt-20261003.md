# Package A serialized NAM/TCN bridge receipt

Package A adds a private CPU bridge for the serialized Neural Amp Modeler
WaveNet/A1 artifact format. The bridge parses and validates the artifact on the
control path, transposes NAM's `[out][in][kernel]` weights into its prepared
private layout, and advances only immutable weights plus preallocated causal
history on the callback. It does not include or link the GPU-NAM checkout and
does not change the public ABI, ModelStore, E126, or MLX paths.

## Provenance

The fixture is copied from the verified sibling checkout
`/Users/danielraffel/Code/pulp-gpu-nam/src/models/example.nam` at detached HEAD
`014f243`. Its SHA-256 is
`66bda2b379289eff079c0755588bc9a92760654d9cc9af1b97cf30d0e92b167d`.
The sibling attribution identifies bundled example models as MIT-licensed
Neural Amp Modeler captures; the sibling `LICENSE.md` SHA-256 is
`35de8f3cebd71bad85e4d9741f3d03cc8deb32349a6568c77c5f4ef11c2911aa` and
`src/models/ATTRIBUTION.md` SHA-256 is
`5be3572325d1dcfbca7740df7b172c2fd9da5e6f22ddc5dc9048ccf5dc71f856`.

## Gates

`pulp build --target pulp-test-nam-tcn-adapter` passed in Release mode. The
focused executable `./build/test/pulp-test-nam-tcn-adapter` passed all 5 test
cases and 29 assertions. The artifact case exercises fresh 64-frame and
128-frame processing, reset replay, and zero callback allocations. The loader
negative control rejects an explicit unsupported state offset; the existing
adapter cases retain oversized-block, non-mono, reset, and allocation controls.

The independent oracle values used by the artifact test begin
`-0.013143630, -0.012158165, -0.014329166, -0.016419834` for the documented
input sequence. Broader architecture coverage (A2, ConvNet, Linear, and
artifact admission through ModelStore) remains outside Package A.
