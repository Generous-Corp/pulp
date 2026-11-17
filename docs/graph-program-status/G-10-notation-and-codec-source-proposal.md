# G-10 spectral notation and codec EOF source: owner decisions

Two G-10 remainders cannot be finished without a design choice that changes a
saved format or adds a public API with new runtime semantics. This file states
each choice precisely so the owner can pick an option. The discoverability part
of the first item is implemented: `pulp graph catalog --json` now lists the four
spectral constructors as `unsupported_typed_api_only` rows with their owning
`SignalGraph::add_spectral_*` constructor, and notation refuses each key with
that constructor instead of telling the author to register a type. The second
item is decided and implemented; section 2 records the decisions.

## 1. Spectral notation and saved-graph persistence

### Current behavior (origin/main)

- `GraphSerializer::to_json` saves a bounded format-6 subset for a graph that
  holds spectral nodes (`core/host/src/spectral_graph_persistence.cpp`): Audio
  boundaries, the two-asset morph with embedded canonical asset bytes,
  identity/mask stages and synthesis. It does not persist analysis nodes, and
  it returns an empty string for a spectral graph outside that subset or one
  the reader cannot prepare. This settled the embedded-versus-sidecar and
  format-version questions below: assets are embedded and spectral graphs are
  written at format 6. See `docs/guides/spectral-asset-replay.md`.
- The strict JSON loader refuses `spectral*` node types and `spectral` edges;
  notation formatting refuses spectral nodes and frame edges
  (`core/host/src/graph_patch.cpp`). The bake path refuses with "spectral graph
  persistence is not admitted" (`core/host/src/baked_graph_processor.cpp`).
- Notation has no constructor-argument syntax. A declaration is
  `name : type [@ version]`; values are set afterwards as `node.param = value`
  against a parameter catalog. A spectral node's identity (FFT size, hop,
  channels, sample rate) is a construction argument, not a parameter.

### What a node needs to round-trip

| Node | State to persist |
| --- | --- |
| analysis, synthesis | identity tuple; sample rate as exact float64 bits |
| stage, `IdentityV1` | identity tuple and kind |
| stage, `RealMaskV1` | identity tuple, kind and `channels * bins` float gains |
| asset morph | two codec streams (up to the 64 MiB runtime bank), explicit source length |

Spectral edges are typed (`PortKind::Spectral`) and already validated by the
authoring path, so edges need only a `spectral` flag on the saved connection.

### Options

1. **Keep spectral graphs runtime-only** (status quo plus the catalog rows).
   No format change. Spectral graphs stay C++-authored.
2. **Persist analysis, stage and synthesis in the saved-graph JSON only.** Add a
   `spectral` object per node with the identity tuple, the stage kind and,
   for `RealMaskV1`, the gain array. Asset morph stays refused. This must ride
   the format v6 writer that G-3 owns rather than extend v5, so it waits on
   that lane. Notation still refuses spectral nodes.
3. **Option 2 plus notation spelling for identity.** Requires a new grammar
   form for construction arguments, for example
   `fft : pulp/spectral_analysis (fft 1024, hop 256, channels 2)`, with the
   sample rate taken from the render or prepare call rather than the text.
   `RealMaskV1` gains are not spelled in notation; a patch with one refuses
   and points to the JSON form.
4. **Option 3 plus asset persistence.** The owner must choose between
   embedding codec bytes (base64 in the document, bounded by the existing
   serializer file limit) and a content-addressed sidecar keyed by the SHA-256
   that `SpectralAssetMorphConfig` already computes.

### Remaining catalog and notation gap

`pulp graph catalog --json` now lists the four spectral constructors. Checked
against `core/host/src/graph_patch.cpp` on origin/main, these gaps remain:

- The spectral rows share one probe identity (FFT 256, hop 64, mono, 48 kHz).
  They do not state the admitted identity range (FFT 256 to 16384 power of two,
  hop 1 to N/2, 1 to 16 channels), and the asset-morph row does not say that its
  geometry is fixed at profile 1, FFT 256, hop 64, mono rather than following
  the probe.
- The stage row is built from `IdentityV1` only; the catalog does not list the
  stage kinds (`IdentityV1`, `RealMaskV1`) or that a mask carries
  `channels * bins` gains.
- `SignalGraphPreparedTopologyEdit` offers analysis, stage and synthesis but no
  asset-morph constructor, so the catalog's asset-morph row has no
  prepared-edit counterpart.
- Notation boundaries are stereo only: formatting refuses an input or output
  boundary that is not two channels, and the default catalog builds both with
  `add_input_node(2)` / `add_output_node(2)`. There is no mono or bus-layout
  boundary spelling.
- Notation has no subgraph-template syntax.
- Persistence of every spectral node is the owner decision in this section.

The first two are additive catalog fields that need no format decision; the
rest depend on the grammar choices above.

### Recommendation

Option 2 now, inside the G-3 v6 writer, with asset morph refused. Decide the
notation grammar (option 3) separately because it changes patch-language v1,
which other notation consumers (G-11 cookbooks, the MCP tool) read.

### Questions for the owner

- Does spectral persistence belong in format v6, or in a later version?
- Is a construction-argument form acceptable in patch-language v1, or does it
  need a version bump of the `# flow` header?
- For assets: embedded bytes or a sidecar with a hash reference?

## 2. Codec-sourced EOF at arbitrary geometry

### Decisions taken

The lead decided this section's choices as follows; the code below implements
them.

1. **Shape of the public path: (b).** `add_spectral_asset_morph_v1` is
   generalized to the decoded stream geometry. No new single-stream source or
   public type was added; a caller that wants a plain source passes the same
   stream twice, with the caveat that morph at amount 0 is not guaranteed to
   reproduce endpoint A bit-exactly.
2. **Clock:** the existing `morph` audio input. One frame per H control
   samples, so the current root path and prepare-time root check are reused.
3. **EOF policy:** the exact-count rule. Each stream holds exactly
   `ceil((L + N - H) / H)` frames for source length L; the finite cutoff is
   `N + H + frames * H`; `finite_frames` is `frame_count + N / H`.
4. **Channels:** every channel count the identity admits, 1 to 16. One control
   value drives all channels.
5. **Hops that do not divide N:** refused. The rule names the refusal
   (`hop_does_not_divide_fft_size`, "hop does not divide the fft size") and the
   public call returns `UnsupportedOperation`.
6. **Persistence:** a complete graph holding the source (source, synthesis and
   output) saves through the format-6 spectral subset. A graph the reader
   cannot prepare, such as the source alone, is still not saveable, and
   `GraphSerializer::to_json` returns an empty string for it.

### Current behavior on this branch

`SpectralAssetMorphConfig::make` decodes asset A, builds
`SpectralFiniteStreamRule` from its geometry (N, H, C) and the caller's length,
then requires asset B to match A's profile, FFT size, hop, channel count,
sample-rate bits and frame count. The rule admits power-of-two N from 256 to
16384, H from 1 to N/2 dividing N, and C from 1 to 16; these are the
`SpectralIdentity` and codec limits plus the dividing-hop restriction. The node
identity, prepare step, per-hop control average, published-frame drain and
routed cutoff all read the rule.

Refusals, each leaving authoring unchanged:

| Cause | Public error | Rule status |
| --- | --- | --- |
| FFT, hop or channels outside the codec range | `InvalidFrame` (the codec cannot carry the asset) | `fft_size_not_admitted`, `hop_not_admitted`, `channels_not_admitted` |
| Hop decodes but does not divide N | `UnsupportedOperation` | `hop_does_not_divide_fft_size` |
| B differs from A in geometry or rate, or a frame count is not the grid count for L | `IdentityMismatch` | ok |
| L is zero or past signed chronology | `InvalidFrame` | `invalid_length` |
| Frame count past signed chronology | `CapacityExceeded` | `capacity_exceeded` |
| Codec resource limit or the 64 MiB runtime bank while decoding either asset | `CapacityExceeded` | ok |

`SpectralFiniteStreamRule::reason()` spells each status.

Realtime safety: endpoint storage is no longer a fixed 129-bin array. A
prepared node sizes two channel-major vectors of `C * (N / 2 + 1)` bins in
`SpectralAssetMorphState::prepare()` on the stopped control thread, charges them
to the runtime bank with the retained assets, and prepares the morph DSP for C
channels. The callback only indexes that storage and a fixed 16-entry pointer
table; the geometry suite renders every partition under the realtime allocation
probe and asserts zero allocations.

### Evidence

`test/test_spectral_asset_geometry.cpp`:

- Renders the source through synthesis at N=256/H=32 mono, N=512/H=128 mono,
  N=1024/H=256 stereo and N=2048/H=1024 three-channel. Assets come from an
  independent double-precision DFT on the grid that
  `spectral_frame_engine_first_frame_start` defines, and each stored frame's
  origin is checked against that grid. Output at amounts 0, 1, 0.37 and a
  varying control is compared with an independent double-precision inverse DFT
  and squared-window OLA oracle (peak 2e-5, RMS 2e-6). The oracle is first
  calibrated to reconstruct the input exactly at both endpoints. Partitions 32,
  64, 128, 4096 and irregular produce identical audio, samples before N + H and
  from `render_frames` on are exactly zero, latency is N + H, and no telemetry
  error is raised.
- Prepares and renders the largest admitted geometry, N=16384/H=8192 with 16
  channels.
- Refuses non-dividing hops (256/100, 512/96 stereo, 1024/3) by name, with a
  dividing hop at the same size as the control; refuses geometry the codec
  cannot carry; refuses channel, hop and frame-count mismatches with
  `IdentityMismatch`; and checks that an incomplete graph holding the source
  is not saveable.
- Bounds retained assets at N=4096/H=2048 with 8 channels: two streams of 240
  frames fit the 64 MiB bank and two of 260 are refused with
  `CapacityExceeded`.

`test/test_spectral_finite_stream_rule.cpp` checks the rule against the
analysis grid at eight geometries (unchanged) and now checks that each refused
geometry carries its own status and that every status has a distinct reason.

### Still open

- The catalog's asset-morph row is built at the probe identity (FFT 256, hop
  64, mono) and does not state the admitted range.
- `SignalGraphPreparedTopologyEdit` has no asset-morph constructor.
- The dedicated single-stream source and its input-free clock are built:
  `SignalGraph::add_spectral_asset_source_v1` (see `S6-codec-source.md`).
  Choice 1(b), passing one stream twice to the morph, is no longer needed for
  a plain source.
- Non-dividing hops are admitted for the single-stream source through the
  versioned finite-stream policy `analysis_grid_v2` (see `S6-codec-source.md`).
  The morph keeps `dividing_hop_v1` because its per-hop control average lines
  up with each frame's window end only when H divides N.
- Paced realtime qualification at large geometry is not measured: a frame at
  N=16384 with 16 channels morphs about 131k bins in one callback.
