# P3 resynthesis consumer receipt (2026-11-13)

Evidence for the P3 admission gate in
`docs/reference/2026-11-12-post-roadmap-audio-graph-capability-gaps.md`
("P3: explicit spectral/frame conversion (conditional)") and the G-10/A3
resynthesis-consumer item. This receipt records one named installed-SDK
consumer. It does not change runtime code or public API, and it does not
promote G-10/A3 beyond the status recorded in `docs/graph-program-status/`.

## Binding

The receipt binds source head `09db01f12ec794c4fa99fdcd3cfbfbe5fae7d23a`: the
merge of `origin/main` into this branch. The commit that updates this file
follows it and changes only this file, so it does not change what the SDK or
the consumer were built from. The staged SDK records that head as clean.

| Item | Value |
| --- | --- |
| Source head | `09db01f12ec794c4fa99fdcd3cfbfbe5fae7d23a` (merge of `origin/main` `d2ffb88af651d135020df74652b88d796348b6b0` into branch head `6c0c2470f7d5`) |
| Consumer | `examples/spectral-resynthesis-consumer/main.cpp` (SHA-256 `bc207ef8c891a3d41b0c0886fc9855a8b3676f787e53a438b5ab50342cf0f808`) |
| Installed SDK | staged by the repository gate from `build/` at the source head. `sdk-provenance.json`: `kind: source-install`, `source_git_sha: 09db01f12ec794c4fa99fdcd3cfbfbe5fae7d23a`, `source_git_dirty: false`, `distribution_eligible: false`, `build_profile: dev`, `platform: darwin-arm64`; `sdk_build_type.txt`: Release; `version.txt`: 1.123.0 |
| Linked target | `Pulp::host` only |
| Consumer executable | Release, 4,095,888 bytes, SHA-256 `e905d9aaed48b12b24d4d052e39906bc0699694845a434463d17c13b6be199c4` |
| Consumer stdout | SHA-256 `74cd41af7e60aa0ddeeee1c2b17b0e37cf6f060c5a40d14258ba461a17f578ee` (the seven receipt lines quoted under Runs) |
| Repository gate | `cmake-spectral-resynthesis-consumer` (`test/cmake/test_spectral_resynthesis_consumer.cmake`, labels `cmake;sdk;host;spectral;slow`) |

The installed `pulp/graph/spectral_frame.hpp`, `pulp/host/spectral_graph.hpp`,
`pulp/host/signal_graph.hpp`, `pulp/host/signal_graph_node.hpp` and
`pulp/host/signal_graph_runtime.hpp` are byte-identical to
`git show 09db01f12ec7:<path>`.

## Consumer shape

Two-channel input -> `add_spectral_analysis` -> `add_spectral_stage`
(`identity_v1`, or `real_mask_v1`) -> `add_spectral_synthesis` -> two-channel
output. Geometry: FFT 512, hop 128, 2 indexed channels, 257 bins, 48 kHz,
profile 1 (symmetric float Hann, local-window phase, squared-window WOLA,
binary32 bins). Source: two bin-centred tones (bins 40 and 120) on channel 0,
a modulated signal on channel 1, an impulse at the last source sample on both
channels, finite length L = 6000 in an 8192-sample render.

## P3 gate checklist

| # | Gate item | Result | Evidence |
| --- | --- | --- | --- |
| 1 | Explicit frame geometry | **PASS** | `SpectralFrameContract` is encoded, decoded and compared field by field (FFT, hop, channels, bins = N/2+1, sample-rate bits, window, phase, normalization, precision). The analysis and synthesis nodes' `spectral_identity` and the analysis node's encoded wire contract equal the declared geometry. Receipt line: `geometry: fft=512 hop=128 channels=2 bins=257 rate=48000`. |
| 2 | Latency = reported PDC | **PASS** | `latency_samples()` = `prepared_latency_snapshot()->total_latency_samples` = 640 = N + H. Measured impulse delay (argmax) is 640 on channel 0 (impulse at 0) and on channel 1 (impulse at 1000), so channel layout is preserved. Instrument control: moving both impulses by one sample moves both readings by exactly one. |
| 3 | EOF/tail | **PASS** (streaming EOF) | Contract (G-10 routed admission row "Tail drain L+N-1"): for a source of length L the output support is `[D, D+L+N-1)` with D the reported latency. Every output sample outside `[640, 7151)` is exactly zero for both identity and masked renders. Last nonzero sample is 7038, past the delayed EOF sample 6639, so the N-1 tail allowance is exercised, not vacuous. EOF is expressed by the host feeding zeros after L samples; the routed graph has no EOF signal. See "Binding gap" for codec-sourced EOF. |
| 4 | CPU fallback | **N/A** | The routed spectral runtime is CPU-only: `git grep -i "gpu\|provider"` over `core/host/src/spectral_graph.cpp`, `spectral_graph_internal.hpp` and `spectral_graph.hpp` at the source head returns no match (control: `fft\|stft\|wola` matches in all three files). There is no GPU path to fall back from. The consumer records `routed_walk_fallbacks() == 0` and bitwise-identical output with parallel routing requested. GPU spectral work (`core/gpu_audio/.../gpu_spectral_morph.hpp`) is a separate kernel that the graph does not reach. |
| 5 | Partition invariance | **PASS** | Output is bitwise identical across partitions 64, 4096, 37 and an irregular 1..4096 schedule, for both the identity and the band-mask graph. Instrument control: the band render differs from the identity render. |
| 6 | Planted geometry mutation | **PASS** | Three CTest controls, each matched on the named failing check (`PASS_REGULAR_EXPRESSION`, not a bare inverted exit): synthesis hop H/2 against analysis hop H is refused at `connect` (`FAIL: stage -> synthesis geometry match`); a window byte of 2 in the encoded contract is refused at decode (`FAIL: geometry contract decode`); a consistent runtime geometry with hop H/2 that disagrees with the recorded receipt fails the PDC check (`FAIL: reported latency N+H`). |

### Reconstruction tolerance and negative control

* Tolerance: per-channel residual energy / reference energy against the input
  delayed by the reported latency must be below 1e-10 (-100 dB).
* Measured identity residual: 1.07e-14 (about -140 dB).
* Negative control (defect present): a uniform `real_mask_v1` gain of
  1 - 1e-4 (a -80 dB defect) measures a residual of 1.0e-8, which exceeds the
  tolerance and is reported as a failure.
* Negative control (defect explained): the same render measured against the
  input scaled by 1 - 1e-4 falls back below the tolerance, so the instrument
  tracks the defect rather than noise.

### Planted spectral transform

`real_mask_v1` with gain 0.25 on bins 32..48 (both channels), 1 elsewhere.
Least-squares tone projection over 4N samples of steady-state output (bin
-centred tones are orthogonal there): tone A (bin 40) measures 0.075000 =
0.25 x 0.3 (tolerance 1e-3 relative), tone B (bin 120) measures 0.200000
(unchanged, tolerance 1e-3). Identity measures tone A at 0.300000.

## Runs

* Repository gate at the source head:
  `ctest --test-dir build -R cmake-spectral-resynthesis-consumer
  --output-on-failure` passed (1/1, 100.8 s) on a Release, examples-OFF
  configure. It staged the SDK with `cmake --install`, configured and built
  the example against the staged prefix, and ran its CTest: 4/4 passed (the
  positive run plus three planted geometry controls, each matched on its named
  failing check).
* Positive run output at the source head:

```text
geometry: fft=512 hop=128 channels=2 bins=257 rate=48000 window=hann.symmetric.v1 normalization=squared-window-wola.v1 layout=indexed
latency: reported=640 snapshot=640 measured_ch0=640 measured_ch1=640
reconstruction: identity_residual=1.07e-14 tolerance=1e-10 planted_-80dB_defect_residual=1e-08
tail: L=6000 support=[640,7151) last_nonzero=7038
transform: toneA identity=0.300000 band=0.075000 (gain 0.2500) toneB=0.200000
cpu: routed_walk_fallbacks=0 parallel_request_identical=1
pulp.spectral-resynthesis-consumer.v1: all checks passed
```

* An earlier development run against a separate source install of
  `4c69e183` produced the same seven lines; it is superseded by the run above.

## Binding gap (codec EOF into a routed source)

Resynthesis is expressible through public API. A codec-decoded stream
(`pulp/signal/spectral_frame_codec.hpp`) cannot be routed into synthesis at an
arbitrary geometry:

* The public spectral sources are `add_spectral_analysis` (audio in) and
  `add_spectral_asset_morph_v1` (`core/host/include/pulp/host/signal_graph_runtime.hpp:222-232`).
* The only codec-to-frames source is the asset morph node, which hard-requires
  profile 1, FFT 256, hop 64, mono and a morph of two assets
  (`core/host/src/spectral_asset_morph_internal.hpp:56-62`; documented at
  `core/host/include/pulp/host/spectral_graph.hpp:35-43`). Its finite tail is
  `render_frames = 320 + frames * 64` (same file, line 67) and is already
  covered by `examples/g10-routed-consumer/asset_morph.cpp`.
* There is no public node that takes a decoded `OwnedSpectralStream` of the
  consumer's geometry and emits frames with codec EOF. This matches the
  acceptance audit, which keeps "Codec EOF-source integration" outside routed
  admission (`docs/graph-program-status/G-10-routed-acceptance.md:48`).

No API was added.

## Recommendation

**P3 admission: ADMIT (scoped).** The gate asks for one named consumer with
frame geometry, partition-invariant offline render, latency receipt, EOF/tail,
CPU fallback and a planted geometry mutation. This consumer provides each item
for the routed audio -> frames -> audio conversion, with CPU fallback recorded
as N/A because the routed path has no GPU provider. The scope is limited to:

* The existing profile-1 analysis/stage/synthesis nodes at one host rate. No
  second scheduler or multirate clock.
* Streaming EOF (host zero padding) with the `[D, D+L+N-1)` support contract.

Codec-sourced EOF at arbitrary geometry and any GPU spectral conversion are
outside this admission. They need their own named consumer. The repository
gate `cmake-spectral-resynthesis-consumer` passed at the source head above.
