# Fused two-asset spectral morph functional evidence

Status: implementation and validation in progress. A3 and G-10 remain PARTIAL.

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
| Governed focused Release build | PASS; first build compiled the routed spectral executable |
| Initial functional run | 5/6 cases passed; reset assertion selected aggregate telemetry node 0 and was corrected to select the source node |
| Existing reconstruction control | PASS, 9,526 assertions |
| Full governed Release build | PASS; complete Release profile, SDKs and Node materialized-runtime registrations present |
| Final focused suite | PASS; 51 cases, 99,156 assertions |
| Installed public consumer | PASS 2/2 locally; exact committed-source provenance refresh pending |
| Production mutations | PENDING; wrong asset, skipped live control, wrong phase, one-sample shift, missing drain |
| Source gates / docs / diff coverage | PENDING; host-refused lanes must remain NOT CHECKED |
| Exact-head unarmed PR | PENDING |

Functional tests cover endpoint/interior/live control, wraparound phase,
DC/Nyquist, isolated impulses at 0/13/final, fixed and irregular partitions,
320-sample silence, full tail, EOF, reset after partial playback/fresh epoch,
asset ownership, malformed canonical records, invalid controls, memory/block
limits, transactional factory refusal and callback allocations. The direct bank
fault lane verifies missing/duplicate/reordered frames and sibling isolation.

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
