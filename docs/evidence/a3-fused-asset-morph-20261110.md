# Fused two-asset spectral morph functional evidence

Status: functional implementation and local validation complete; unarmed publication
pending. A3 and G-10 remain PARTIAL.

The admitted functional boundary is a closed, finite mono profile-1 asset source
(FFT 256, hop 64) with one direct host Audio control input. It reuses
`SpectralMorphT`, the canonical codec, typed stages, synthesis and serial executor.
This change does not modify event transport, binding, SDK archive production,
morph DSP, codec format or generic format execution.

The control-thread factory validates and owns both assets, hashes canonical
inputs, verifies equal geometry/rate/grid/count and checks length/cutoff arithmetic.
Preparation charges immutable storage, fixed endpoint scratch, configuration and
edges against the existing 64 MiB bound. The callback averages consecutive
64-sample control hops across host partitions. Four zero frames drain synthesis;
finite extent propagates through stages and synthesis, then rendering stays silent.

The public consumer's glue and independent oracle are in
`examples/g10-routed-consumer/asset_morph.cpp` and `asset_morph_oracle.hpp`.
The expected audio uses direct float64 DFT, shortest-arc phase interpolation,
explicit symmetric Hann, direct IDFT and overlap-add normalization. It does not
call production FFT, morph or OLA for expected audio. Frozen tolerances are
absolute peak error <= 2e-5 and RMS error <= 2e-6; endpoint delayed identity is
calibrated to 1e-11 before candidate comparison.

## Validation ledger

| Lane | Current evidence |
| --- | --- |
| Governed complete Release build | PASS; complete dev profile, GPU enabled, examples OFF, VST3/AUSDK and Node registrations present |
| Restored routed suite | PASS; 51 cases, 99,156 assertions |
| Installed CMake consumer gate | PASS 1/1; nested public consumers PASS 2/2 |
| Neighbor controls | PASS; trigger-runtime public consumer and DSPX-04 fail-closed processor binding |
| Production mutations | All five CONFIRMED; compiled oracle test exits 0/42/0, driver exits 0 |
| Source gates | Exposure path corrected; protected-base metadata repaired by clean merge of main f2664397c643807f01a4a885687ef6ebd4d30efa, preserving narrow projections without regeneration. Consumer TU fan-out declarations added; final head rerun required. Host-refused suites remain NOT CHECKED |
| Local diff coverage | NOT CHECKED, exit 4; dedicated-builder policy refusal |
| Capability projections / docs | PASS on implementation head; narrow delegated projections only |
| Script inventory | Correct-profile generation retained all off-profile records; only unrelated UI fixture advisory drift produced and excluded |
| Exact-head unarmed PR | PENDING |

Functional tests cover endpoint/interior/live control, wraparound phase,
DC/Nyquist, isolated impulses at 0/13/final, fixed and irregular partitions,
320-sample silence, full tail, EOF, reset after partial playback/fresh epoch,
asset ownership, malformed canonical records, invalid controls, memory/block
limits, transactional factory refusal and callback allocations. The direct bank
fault lane verifies missing/duplicate/reordered frames and sibling isolation.

## Reproducible functional receipts

Implementation source: `2a5a98e3ab69ecc20fe86a3840fb87298fcd0396`.
The installed SDK's `build_info.hpp` records that full SHA and `kGitDirty=false`.
This is a local Release SDK installation, not an official published SDK archive.
The consumer links only `Pulp::host` and includes public Pulp headers; its main
and independent oracle total 194 non-generated lines. It renders live control
under 32/64/128 and irregular partitions, with bit-identical audio.
Measured peak error is 8.06619825932664e-8 and RMS 6.795748513085668e-9.

Commands (all build commands governed):

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_EXAMPLES=OFF
tools/ci/governed-build.sh cmake --build build
build/test/pulp-test-routed-spectral-ports
ctest --test-dir build -R '^cmake-spectral-asset-morph-consumer$' --output-on-failure
build/asset-morph-consumer-smoke/build/asset-morph-consumer build/a3-evidence/consumer
tools/scripts/gates.sh origin/main
```

The installed consumer gate keys its staging prefix by the producer install
identity, preserving immutable source-install provenance across reruns. A prior
fixed-prefix rerun correctly refused to replace an older marker; that harness
state failure did not enter the audio oracle. The gate stages the complete SDK,
configures the external
example with `find_package(Pulp COMPONENTS host)`, governs its Release build and
runs both public consumers. Input/output blobs are little-endian float32/float64
on this arm64 macOS host. SHA-256 receipts at the implementation source:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| asset-a.sfrm | 16832 | `0f7a92fc41b1befc24981979e9b1cab6fc6f27d0527caa3285315ecdaac2f5ff` |
| asset-b.sfrm | 16832 | `52a37e77f8f7fbe8547d940b93e9ab6a08d0550d7651cd4cebb930d1136b87d7` |
| control.f32 | 8192 | `87e4c02c814455f69c9d2969d222752739c7b6ccc5c0aa18cc479246a67ae815` |
| expected.f64 | 16384 | `a04896e9201f8c049fa145f291bba89bf5f8bc8be60e7632389542457caab989` |
| output.f32 | 8192 | `76553b80d5d0afdf7e99fd952fdcfbc9ea649603358c324d191925d07532d3b9` |
| asset-morph-consumer | 3460792 | `d78af91aa66e253d8953ce9f32d45718d7af68e04c39f80cfe2960fa71b93689` |
| installed libpulp-host.a | 11590880 | `5aac28ed3269752277c1b4effe660801ec8e2b88130202dafe69aa52a9e2777b` |
| build_info.hpp | 1734 | `5fda9babd8bd1ef730dc71e796e079eb543157c6e550dfd96fa081874ffe4f02` |

Mutation selector: `build/test/pulp-test-routed-spectral-ports '[asset-audio-oracle]'`.
Each `tools/scripts/confirm_failure.sh` run used `--build-dir build --target
pulp-test-routed-spectral-ports --jobs 2 --keep-logs` and a test wrapper that
prints and preserves the actual numeric test exit. Each edit compiled, changed
the executed binary, failed the independent audio oracle, restored exact tracked
bytes and passed again. No build failure counts as proof.

| Production mutation | Baseline / broken / restored | Example broken peak / RMS | Driver |
| --- | --- | --- | --- |
| wrong-asset | 0 / 42 / 0 | peak=0.010715 rms=0.003087 | 0 |
| skipped-live-control | 0 / 42 / 0 | peak=0.007836 rms=0.002113 | 0 |
| wrong-phase | 0 / 42 / 0 | peak=0.011481 rms=0.003317 | 0 |
| one-sample-shift | 0 / 42 / 0 | peak=0.555994 rms=0.043783 | 0 |
| missing-drain | 0 / 42 / 0 | peak=0.460104 rms=0.010167 | 0 |

Edits were: read B endpoints from A; replace both amounts with zero; replace only
phase amount with zero; add one to synthesis latency; remove all four zero drain
frames. The drain control catches missing normalization and tail audio, not just
a diagnostic. Local phase logs are retained under `build/a3-evidence/mutations/`.
Restoration was additionally verified with `git diff --exit-code` and SHA-256:

- `core/host/src/spectral_asset_morph_internal.hpp`: `1b4e95e62606984352d43343c74b932ca45e1854e6379f713d8339044b8dd501`
- `core/host/src/spectral_graph_internal.hpp`: `4e0c1ed85e95adf6dcec359901d6bab49aec820d782ddf34915bc04284ade909`
- `core/host/src/spectral_graph.cpp`: `ba4f77eb5514ffbf5e361614135c7b4247f84f80fb6191ba6a56e6c57adbbe04`

The original antipodal failure log is retained as
`build/a3-evidence/antipodal-original-dft-failure.log` (1,046 bytes, SHA-256
`ee4c2ab6420533ee76313059c317922bb2b517fcaf51f7844364c2a73ee48550`).

## Remaining qualification

An original-double-DFT antipodal impulse (+0.5/-0.25 at source sample 0, control
0.37) produced peak error 0.066874 and RMS 0.001398. The original DFT and
float32 serialized-bin phases can choose opposite equally short arcs at the
exact-pi discontinuity of `remainder`. The failure is retained locally in the
build evidence. Tolerances and the existing morph primitive remain unchanged.
Chronology uses non-antipodal impulses with a unique expected answer; separate
canonical real-bin antipodal tests cover analytic half-turn branches, endpoints
and bit-identical callback partitions. This does not define a new cross-platform
unique tie rule for arbitrary quantized original DFTs.

Versioned asset references/original-length metadata, bounded saved-graph
serialization, notation and ctest replay remain required follow-up work.
The original paced 44.1/48/96 kHz by 32/64/128/256 callback matrix and its
jitter/budget bounds, plus supported sanitizer/mutation lanes, remain required.
Functional and allocation evidence does not establish realtime qualification
or completion of the full A3 phase, broader spectral analysis or granular work.
