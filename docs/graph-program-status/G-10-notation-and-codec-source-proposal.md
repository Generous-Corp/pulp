# G-10 spectral notation and codec EOF source: owner decisions

Two G-10 remainders cannot be finished without a design choice that changes a
saved format or adds a public API with new runtime semantics. This file states
each choice precisely so the owner can pick an option. The discoverability part
of the first item is implemented: `pulp graph catalog --json` now lists the four
spectral constructors as `unsupported_typed_api_only` rows with their owning
`SignalGraph::add_spectral_*` constructor, and notation refuses each key with
that constructor instead of telling the author to register a type.

## 1. Spectral notation and saved-graph persistence

### Current behavior (origin/main)

- `GraphSerializer::to_json` returns an empty string for any graph that holds a
  spectral node (`core/host/src/graph_serializer.cpp`, the `spectral_identity`
  check at the top of `to_json`).
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

### Current behavior (origin/main)

The only node that turns codec bytes into routed frames is
`SignalGraph::add_spectral_asset_morph_v1`. Its geometry is fixed at profile 1,
FFT 256, hop 64, mono, in each of these places:

- `core/host/src/spectral_asset_morph_internal.hpp`: `make()` refuses any other
  geometry; `frames = (length + 192 + 63) / 64` and
  `render_frames = 320 + frames * 64` hard-code N=256 and H=64; `frame()`
  reads 129 bins with a stride of 258 components.
- `core/host/src/spectral_graph.cpp`: the node identity is built as
  `SpectralIdentity::make(256, 64, 1, rate)`; prepare calls
  `morph.prepare(1, 129)` and sets `finite_frames = frame_count + 4` (4 is N/H).
- `core/host/src/spectral_graph_internal.hpp`: the callback averages exactly 64
  control samples per frame.

The codec itself (`core/signal/include/pulp/signal/spectral_frame_codec.hpp`)
already accepts FFT 256 to 16384 (power of two), hop 1 to N/2 and 1 to 16
channels, which matches `SpectralIdentity::make`. Preparation accepts a
spectral root only if it is an analysis or asset morph node.

### Prerequisite already in place

The end-of-stream arithmetic is one internal, geometry-parameterized rule,
`SpectralFiniteStreamRule` in `core/host/src/spectral_finite_stream_internal.hpp`.
It derives the frame count, routed cutoff, published frame count (`N / H` drain),
bin count and per-frame, per-channel component offsets from (N, H, C, L) with
checked arithmetic against signed chronology. The asset morph node, its prepare
step and its per-hop control average now read every one of those values from the
rule instead of the literals listed above. The admitted geometry is unchanged:
`make()` still refuses anything but profile 1, FFT 256, hop 64, mono, and the
morph endpoint storage is still sized for 129 bins. No public API, port or
format changed.

`test/test_spectral_finite_stream_rule.cpp` checks the rule against the
analysis frame grid the engine starts on
(`spectral_frame_engine_first_frame_start`), not against its own formula, at
eight geometries from N=256/H=1 to N=16384/H=8192 with 16 channels, plus the
exact profile-one constants for lengths 1 to 4096, the component tiling, the
length and capacity bounds at each geometry, and the refused geometries.

The rule found a defect in the EOF policy proposed in choice 3 below: the
closed form `ceil((L + N - H) / H)` equals the number of grid frames that
overlap the source only when H divides N. For N=256, H=100, L=1 the closed form
gives 2 frames while the analysis grid starts at -200 and produces 3. The codec
and `SpectralIdentity` both accept such hops. The rule therefore refuses a hop
that does not divide N; choice 5 asks whether that restriction should stand.

### Design choices that need a decision

1. **Shape of the public path.**
   - (a) A new single-stream source,
     `add_spectral_stream_source_v1(encoded, source_samples, name)`, with one
     `frames` output at the decoded geometry.
   - (b) Generalize `add_spectral_asset_morph_v1` to any matching geometry and
     let a caller pass the same stream twice. This is cheaper, but morph at
     amount 0 is not guaranteed to reproduce endpoint A bit-exactly, so it is
     not a faithful source.
   Recommendation: (a).
2. **Clock.** Every current spectral root consumes host audio, which is what
   advances its frame clock. An input-free source needs either a clock or
   trigger audio input (like asset morph's `morph` port: one frame per H input
   samples) or an executor change so a root can advance from the callback's
   sample count alone. Recommendation: an audio `clock` input, because it
   reuses the existing root path and prepare-time root check.
3. **EOF policy.** Generalize the asset morph rule: the stream must hold exactly
   `ceil((L + N - H) / H)` frames for source length L; frames past the end are
   zero; the finite cutoff is `N + H + frames * H` samples; `finite_frames` is
   `frame_count + N / H`. An alternative is to accept any frame count and
   derive L, which loses the explicit length check. Recommendation: keep the
   exact-count rule.
4. **Channels.** Allow the decoded channel count (1 to 16) or restrict to mono
   first. Recommendation: allow all, because synthesis already takes the
   identity's channel count.
5. **Hops that do not divide N.** Either keep the source restricted to
   power-of-two hops (H divides N), which is what the rule admits today, or
   define the frame count as the analysis-grid count
   `floor((L - 1) / H) + floor((N - 1) / H) + 1` and the drain as
   `ceil(N / H)`, then prove the cutoff against the synthesis engine at a
   non-dividing hop. Recommendation: keep the restriction for the first public
   source; every current consumer and evidence row uses H dividing N.

### Work once decided

Add the node type and ports in `spectral_graph.hpp`, `signal_graph_node.hpp`
and `signal_graph_runtime.hpp`; construct the source's `SpectralFiniteStreamRule`
from the decoded geometry; size the per-node frame storage from `rule.bins` and
`rule.channels` instead of the fixed 129-bin array; add the callback branch; update
`graph_rt_report.cpp`, the agent capability manifest, the consumption census
and the catalog row added here. Acceptance: an analytic round trip through
synthesis at two non-default geometries (for example N=1024/H=256 stereo and
N=512/H=128 mono) checked against an independent float64 IDFT/OLA reference;
exact prefix and tail extent; 32/64/128 and irregular partitions identical;
and planted mutations of the frame-count rule, cutoff, N/H tail and channel
stride each caught by `confirm_failure.sh`.
