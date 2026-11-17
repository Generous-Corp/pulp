# S6: single-stream codec source and non-dividing finite geometry

Status of the two S6 slices, checked against the code on this branch.

## Slice 1: single-stream source

`SignalGraph::add_spectral_asset_source_v1(asset, source_samples, name)` takes
one encoded profile-1 codec stream and its exact source length. It decodes the
stream once on the stopped control thread through the same decoder, finite
rule and runtime bank the two-asset morph uses
(`decode_finite_spectral_stream` in
`core/host/src/spectral_asset_stream_internal.hpp`), and retains one immutable
`SpectralAssetSourceConfig`. There is no morph amount and no second asset.

- **Clock.** The node has no inputs. Frame f is published in the host block
  whose end reaches the frame's window end, `origin + N`. That is the block in
  which analysis publishes the same frame and in which synthesis first
  consumes it, so the source advances on the host-block sample timeline. There
  is no wall clock and no autonomous scheduler.
- **Payload.** Stored bins are copied into the node's fixed working storage
  and published unchanged. A stored stamp that does not sit at the runtime
  origin, or a nonfinite bin, is a fatal `InvalidFrame`.
- **EOF.** `SpectralAssetSourcePorts::end` reports the policy version, source
  length, grid frame count, drain frame count and render cutoff. All come from
  `SpectralFiniteStreamRule` with checked arithmetic. A stored frame count
  other than the grid count for the stated length is `IdentityMismatch`; a
  zero length is `InvalidFrame`.
- **Resources.** The decoded stream is charged to the 64 MiB runtime bank at
  authoring and again at prepare. One stream of 480 frames at N=4096, H=2048,
  8 channels fits where the morph's two do not; the codec's largest such
  stream (511 frames) is admitted but refused at prepare.
- **Persistence.** Out of scope. The writer refuses a graph holding the source
  with a named reason rather than dropping the node.

## Slice 2: finite geometry for non-dividing hops

`SpectralFiniteStreamRule` now carries a versioned policy.

| Value | Formula |
| --- | --- |
| frame count | `floor((L - 1) / H) + floor((N - 1) / H) + 1` |
| drain | `ceil(N / H)`, checked |
| cutoff | `(N + H) + floor((L - 1) / H) * H + N`, from L alone |
| length bound | `L + N - 1` fits signed chronology |
| capacity bound | `cutoff + N` fits signed chronology |

`dividing_hop_v1` produces the same values and refuses a hop that does not
divide N. `analysis_grid_v2` admits every hop from 1 to N / 2 and is the
default. At every dividing geometry both policies equal the earlier closed
forms (`ceil((L + N - H) / H)` frames, `N / H` drain, `N + H + frames * H`
cutoff), including the length and capacity boundaries.

The single-stream source uses v2. The morph and its saved assets stay on v1:
frame f averages control hop f and is published at sample `(f + 1) * H`, but
synthesis needs it at its window end, which is earlier when H does not divide
N. Admitting non-dividing hops for the morph needs a new control-window
contract (for example, the H samples before the window end, with a shorter
first window); that is not built.

## Evidence

- `test/test_spectral_finite_stream_rule.cpp`: count, drain and cutoff against
  a walked grid at hops 1, 3, 24, 63, 64, 96, 100, 128, 256, 1000, 2048, 8191
  and 8192; the drain proven minimal and sufficient against guarded synthesis;
  both policies against the earlier closed forms; policy and geometry
  refusals; length and capacity boundaries.
- `test/test_spectral_asset_source.cpp`: renders through the public graph at
  hops 3, 63, 64, 96, 100, 128 and 256, 1 to 3 channels, lengths 1, 2, H - 1,
  H, H + 1, 2H, 2H + 1, N - 1, N, N + 1, N + H and longer, partitions 1, 17,
  32, 63, 64, 4096 and ragged. Output is compared with an independent
  double-precision inverse DFT and squared-window OLA oracle (calibrated to
  reconstruct the input), bit for bit with direct guarded synthesis of the
  stored frames, and is exactly zero before N + H and from the cutoff. Every
  callback is checked for zero allocations, and telemetry stays clean.
- `examples/spectral-resynthesis-consumer`: the installed-SDK consumer
  analyzes its own stereo audio at FFT 512 / hop 96, feeds the single stream
  to synthesis and checks the reported end against its own walked grid, with
  planted length, frame-count and drain mutations that must fail.

## Not done

- Persistence of the single-stream source (D6 follows D1).
- Non-dividing hops for the two-asset morph (see above).
- Paced realtime qualification at large geometry. The largest admitted shapes
  prepare and render silence without callback allocations, but no callback
  timing is measured.
- A `pulp graph catalog` row and a `SignalGraphPreparedTopologyEdit`
  constructor for the source.
