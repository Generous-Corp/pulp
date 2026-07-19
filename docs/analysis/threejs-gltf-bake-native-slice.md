# Three.js glTF/Bake Native Slice

Date: 2026-06-03
Branch: `feature/threejs-gltf-bake-native-spikes`
Tracking issue: https://github.com/Generous-Corp/pulp/issues/2738

## Scope

This branch owns the native no-JS and bake-architecture lane from
`planning/threejs-webgpu-gltf-bake-plan.md`.

Owned work:

- Native module boundary for glTF/GLB scene data and no-JS rendering.
- fastgltf/cgltf decision work behind the native scene-data boundary.
- `SceneData` shape, sidecar diagnostics/provenance schema, and bake/export
  architecture.
- Native hardcoded textured-cube renderer proof with offscreen color, depth,
  and RGBA readback.
- Native `BoxTextured.glb` rendering after the renderer proof exists, a native
  loader slice exists, and the shared fixture decision is coordinated through
  the issue tracker.

Not owned here unless explicitly delegated through an issue:

- Live Three.js `GLTFLoader`, `TextureLoader`, `DRACOLoader`, `KTX2Loader`, or
  `GLTFExporter` web-compat bridge work.
- `core/view` runtime hardening, plugin-editor bridge work, or JS module-loader
  changes.
- Shared GLB fixtures or render/CMake/dependency edits without a tracking issue
  comment describing the cross-lane impact first.

The requested June 3 runtime-hardening and parallel-coordination docs were not
present in the pinned `planning` submodule at commit
`fc917a3b2fcd76870a581964fe1aaf2f6a2f7a29`, but they were read from
the planning submodule's `origin/main`. The native and runtime-hardening lanes
remain separate coordination tracks.

## Re-verified Current State

The current branch still matches the plan's native-renderer risk model:

- `core/render` creates compute pipelines in `gpu_compute.cpp`; no native
  mesh/material/scene render-pipeline abstraction exists there.
- Render pipelines in `core/view/src/widget_bridge.cpp` are WebGPU command
  execution driven by JS-generated WGSL and payloads, not reusable native scene
  rendering.
- `examples/threejs-native-demo/main.cpp` resolves `three`, `three/webgpu`,
  `three.core.js`, and `OrbitControls.js`, but not GLTFLoader-family modules.
- `core/view/js/web-compat-observers.js` still installs a no-op
  `XMLHttpRequest` stub.
- No `toDataURL` or `toBlob` implementation was found in the web-compat canvas
  path.
- `core/render/src/draco_decoder.cpp` now has a decode-by-Draco-unique-id
  overload for the glTF `KHR_draco_mesh_compression` semantic map, while the
  old named-attribute decoder remains for existing callers. `core/scene` now
  routes compressed bufferView bytes and semantic unique IDs through an injected
  Draco decode callback, and `core/render/include/pulp/render/draco_scene_adapter.hpp`
  provides the concrete renderer-decoder callback adapter without adding a
  `pulp-scene` -> `pulp-render` link cycle.
- `pulp-scene3d-inspect-native --native-draco` is a render-owned inspector
  variant that opts into that render-side callback and prints
  `native_draco_decoder_available=true|false`, so diagnostic runs distinguish a
  compiled-out Draco decoder from an unwired loader boundary. The loader emits
  `gltf.draco_unavailable` for Draco-required assets when that callback reports
  a compiled-out decoder, without reversing the `pulp-render` -> `pulp-scene`
  dependency.
- `core/render/src/ktx2_decoder.cpp` still reads header fields but does not
  transcode KTX2/Basis payload pixels.
- Before this branch's implementation, no `PULP_ENABLE_SCENE3D`,
  fastgltf/cgltf/tinygltf dependency, native `SceneData`, native `Renderer3D`,
  or checked-in `.glb` fixture existed yet.

## Module Boundary Decision

Use a new opt-in top-level module: `core/scene/`.

Rationale:

- `SceneData` is CPU-side normalized asset data, not a Dawn resource owner.
- The loader boundary must not leak fastgltf structs or GPU objects.
- `core/render` remains the GPU subsystem; the native renderer can consume
  `core/scene` data through a thin render-facing API.
- `PULP_ENABLE_SCENE3D` stays off by default so non-3D plugins do not pay the
  fastgltf/simdjson/native-renderer cost.

Initial boundary:

- `core/scene/include/pulp/scene/scene_data.hpp`
  - Nodes, meshes, primitives, accessors decoded to engine-owned CPU buffers,
    materials, textures-as-bytes, cameras, lights, diagnostics.
- `core/scene/include/pulp/scene/gltf_loader.hpp`
  - Parses GLB/gltf into `SceneData`.
  - fastgltf is private implementation detail.
  - Draco hooks are optional and emit explicit diagnostics when unavailable.
- `core/scene/include/pulp/scene/sidecar.hpp`
  - `*.pulp3d.json` schema for diagnostics, provenance, unsupported features,
    and runtime hints.
- `core/render/include/pulp/render/renderer3d.hpp`
  - Owns GPU resources, pipelines, depth targets, offscreen targets, and
    readback.
  - Consumes `SceneData`; does not know fastgltf structs.

## Spike B Renderer Proof Scope

The first native code spike is a hardcoded textured cube proof. It deliberately
does not parse glTF, link JS, run Three.js, or depend on live GLB evidence.

Required behavior:

- Build only when `PULP_ENABLE_SCENE3D=ON`.
- Create an offscreen color target and depth target without using
  `GpuSurface::begin_frame()`.
- Use the existing Dawn device/queue patterns from `core/render`, especially
  buffer creation, queue writes, and readback patterns.
- Compile one WGSL shader with vertex + fragment stages.
- Upload hand-written indexed cube geometry, UVs, one small texture, sampler,
  transform uniforms, and draw with depth testing.
- Read back RGBA bytes and write a PNG for human inspection.

Exact validation:

- Structural assertions:
  - color target dimensions match the requested render size.
  - depth target is allocated and bound.
  - vertex, index, uniform, texture, and readback buffers are non-empty.
  - command submission succeeds without Dawn uncaptured errors.
  - readback contains more than one color and has non-transparent foreground
    pixels.
- Linkage assertion:
  - the proof target must not link `pulp_view`, V8, QuickJS, JSC, Three.js, or
    web-compat JS assets.
- Visual assertion:
  - generate a macOS-first PNG golden for the hardcoded cube.
  - defer cross-platform deterministic GPU goldens until software-adapter
    infrastructure exists.

## Spike B Implementation Status

Implemented in this branch:

- `PULP_ENABLE_SCENE3D`, default `OFF`.
- `core/scene/` CPU-only scene module with `SceneData`, loader-boundary,
  sidecar schema/JSON helpers, and a private fastgltf-backed GLB loader.
- `pulp::render::Renderer3D::render_hardcoded_textured_cube()`.
- `pulp::render::Renderer3D::render_scene_data()`, which consumes normalized
  CPU `SceneData` and renders the first indexed primitive through the native
  Dawn path.
- A generated, fixture-free textured cube GLB test path that exercises
  fastgltf -> `SceneData` -> `Renderer3D` with 24 cube vertices, normals, UVs,
  36 indices, embedded PNG bytes, and sampler metadata. This is a native
  BoxTextured-like proof without creating or editing a shared fixture.
- A generated multi-node GLB render test that parses two root nodes referencing
  the same textured mesh, then proves the native render-packet join emits and
  draws two primitives through one renderer pipeline-cache entry. This pins the
  loader-to-renderer scene graph path beyond the single-node BoxTextured
  fixture without touching shared runtime fixtures.
- The official Khronos `BoxTextured.glb` fixture at
  `test/fixtures/scene3d/BoxTextured/BoxTextured.glb`, added after #2738
  shared-fixture intent. The native loader and renderer now use it as the
  Phase 6b join proof: official GLB -> fastgltf -> `SceneData` ->
  `Renderer3D`.
- `tools/scene3d/verify_boxtextured_fixture.py`, wired through
  `scene3d-boxtextured-fixture-contract`, pins the shared fixture's SHA-256,
  upstream source URLs, license markers, README text, `DEPENDENCIES.md`,
  `NOTICE.md`, `docs/reference/licensing.md`, and `tools/deps/manifest.json`
  entry so the coordinated binary fixture cannot drift silently.
- `scene3d-boxtextured-fixture-negative-contract` feeds temporary repo copies
  through that verifier so it must reject fixture hash drift, README/source URL
  drift, dependency license drift, NOTICE marker drift, licensing path drift,
  manifest version/source-file drift, and a missing manifest entry.
- `pulp-test-renderer3d`, built only when `PULP_ENABLE_GPU=ON` and
  `PULP_ENABLE_SCENE3D=ON`.
- `pulp-test-scene3d`, validating CPU scene references and generated embedded
  GLB parsing without GPU resources. SceneData validation coverage now pins all
  five material texture slots and all five sampler slots so future loader
  expansion cannot validate only the base-color path by accident.
- `scene3d-scene-data-surface-contract` statically pins the central native
  `SceneData` substrate: renderer-critical `PrimitiveData` field order, the
  top-level `SceneData` table order, `SceneData::empty()` /
  `is_valid_scene_index()` behavior, and the ordered
  `validate_scene_data()` diagnostic-code surface used by stats, sidecar,
  render-packet, and renderer handoff checks.
- `scene3d-scene-data-surface-negative-contract` feeds temporary header
  mutations through that same verifier so the guard must reject primitive-field
  drift, top-level scene-table order drift, validation diagnostic-code drift,
  `SceneData::empty()` drift, invalid-index sentinel drift, and diagnostic
  severity-name drift.
- `tools/scene3d/verify_scene3d_inspect.py`, wired through
  `scene3d-inspect-boxtextured-contract`, runs `pulp-scene3d-inspect
  --render-packet` against the official BoxTextured fixture and asserts the
  complete stable stats/render-packet contract, rejecting missing, duplicated,
  mismatched, or extra fields while pinning node/root/mesh/primitive counts,
  vertices, indices, material/texture/sampler counts, texture byte count,
  advanced material extension count, zero diagnostics, transformed-node count,
  primitive material key, feature mask, and named render features.
- `tools/scene3d/verify_scene3d_inspect_contract.py`, wired through
  `scene3d-inspect-verifier-contract`, feeds synthetic inspect output through
  the same verifier to prove missing render-packet rows, duplicate primitive
  rows, stats drift, and primitive feature-mask drift are rejected.
- `tools/scene3d/verify_scene3d_sidecar.py`, wired through
  `scene3d-sidecar-boxtextured-contract`, runs `pulp-scene3d-sidecar` against
  the official BoxTextured fixture and parses the emitted JSON instead of
  relying on a broad regex. It asserts schema version 1, exact provenance
  (`source`, `exporter`, `exported_at`, `runtime_evidence`), exact root and
  provenance key sets, and empty diagnostics / unsupported-feature /
  runtime-hint arrays for the clean native fixture. The generated sidecar tests use a runtime-hardening
  issue/comment URL for `runtime_evidence`, keeping live GLB proof ownership in
  the runtime lane rather than the native tracking issue.
- `tools/scene3d/verify_scene3d_sidecar_contract.py`, wired through
  `scene3d-sidecar-verifier-contract`, feeds synthetic sidecar JSON through the
  same verifier to prove missing `runtime_hints`, extra root keys, provenance
  exporter drift, unexpected diagnostics, and malformed JSON are rejected.
- `tools/scene3d/verify_sidecar_schema_constants.py`, wired through
  `scene3d-sidecar-schema-constants-contract`, keeps the duplicated sidecar
  schema key sets in the BoxTextured sidecar verifier, bake-artifact verifier,
  and bake-preflight matrix verifier aligned so those public handoff tools
  cannot quietly accept different JSON shapes.
- `scene3d-sidecar-schema-constants-negative-contract` feeds temporary verifier
  mutations through that alignment checker so it must reject canonical root-key
  drift, missing canonical constants, preflight matrix root/diagnostic key drift,
  missing preflight constants, and BoxTextured provenance/root constant drift.
- `tools/scene3d/verify_bake_preflight_matrix.py`, wired through
  `scene3d-bake-preflight-matrix-contract`, runs `pulp-scene3d-bake-preflight`
  across the checked-in sidecar fixture matrix and asserts each fixture's JSON
  schema/provenance shape, diagnostic code order, unsupported-feature rows,
  exact readiness, blocker booleans, bucket counts, exit codes, and
  feature/node rows, plus exact runtime-hint key/value pairs. It also checks
  that every checked-in `*.pulp3d.json` sidecar fixture is listed in the matrix
  and every listed fixture exists, so new fixture rows cannot bypass the CLI
  preflight contract. `verify_bake_preflight_matrix_fixture_set.py`, wired
  through `scene3d-bake-preflight-matrix-fixture-set`, pins that drift guard by
  creating temporary unlisted and missing fixture sets and requiring both to
  fail. This keeps clean default-path PBR material floors, export
  blockers, texture-encoding blockers, native runtime gaps, and missing runtime
  evidence separate without depending on live Three.js runtime code.
- `tools/scene3d/verify_native_renderer_boundary.py`, wired through
  `scene3d-native-renderer-link-boundary`, verifies the build-generated
  `link.txt` files for the native Scene3D inspect/sidecar/preflight CLIs, the
  render-owned native inspector/probe, and the Scene3D/Renderer3D tests do not
  link `pulp_view`, JS engines, Three.js runtime assets, the live native demo,
  or web-compat JS. It also checks the positive native dependency contract:
  Scene3D CLI/test targets must link `pulp-scene` plus fastgltf/simdjson, while
  Renderer3D/native-inspect targets must also link `pulp-render` and the native
  GPU/image stack. This turns the no-JS linkage assertion into an automated
  boundary check across the native bake surface.
- `scene3d-native-renderer-link-boundary-negative-contract` feeds temporary
  `link.txt` mutations through that checker so it must reject view-module
  linkage, `WidgetBridge` linkage, missing fastgltf linkage, missing native
  WebGPU linkage, and missing required link files. This keeps the link boundary
  from becoming a positive-only smoke.
- `tools/scene3d/verify_public_scene3d_boundary.py`, wired through
  `scene3d-public-boundary`, enforces the exact public Scene3D header allowlist
  and scans those headers with comments stripped. It fails if a new public
  Scene3D header appears without updating the boundary contract, if an expected
  header disappears, or if fastgltf/cgltf/tinygltf types, Dawn/WebGPU/Skia
  implementation types, `pulp::view`, JS engine types, Three.js runtime names,
  or web-compat bridge symbols leak across the public loader/renderer boundary.
- `scene3d-public-boundary-negative-contract` feeds temporary header sets and
  leak cases through that same verifier, proving it rejects missing or extra
  public headers and representative parser, Dawn/WebGPU, Skia, view, JS engine,
  Three.js, and web-compat symbols before such leaks can become part of the
  native public API.
- `tools/scene3d/verify_native_source_boundary.py`, wired through
  `scene3d-native-source-boundary`, scans the native-owned implementation
  sources with comments and string literals stripped. It keeps `core/scene/src`
  CPU-side by forbidding Dawn/WebGPU/Skia implementation surfaces there, allows
  fastgltf only inside `gltf_loader.cpp`, keeps Renderer3D source independent
  of fastgltf/cgltf/tinygltf structs, and rejects accidental references to
  `pulp::view`, `WidgetBridge`, JS engines, live Three.js loader/exporter
  symbols, or web-compat runtime assets.
- `tools/scene3d/verify_scene3d_cmake_boundary.py`, wired through
  `scene3d-cmake-boundary`, verifies the CMake opt-in itself: enabled builds
  produce the Scene3D module/test/probe target directories plus generated
  `link.txt` metadata for the native Scene3D CLI/probe/test targets, while
  disabled builds do not generate `core/scene`, Renderer3D probe/tests,
  Scene3D CLI target metadata, or Scene3D CLI target artifacts.
- `scene3d-cmake-boundary-negative-contract` feeds synthetic enabled and
  disabled build directories through the same verifier so it must reject cache
  expectation mismatches, missing enabled build paths, missing enabled link
  files, disabled build-path leakage, disabled generated-link leakage, and
  disabled CLI target leakage.

Validation run:

```bash
cmake -S . -B build-scene3d-skia \
  -DCMAKE_BUILD_TYPE=Release \
  -DPULP_ENABLE_SCENE3D=ON \
  -DPULP_BUILD_EXAMPLES=OFF \
  -DSKIA_DIR=/Volumes/Workshop/Code/pulp/external/skia-build \
  -DPULP_FETCHCONTENT_UPDATES_DISCONNECTED=ON

cmake --build build-scene3d-skia --target pulp-test-renderer3d -j8
./build-scene3d-skia/test/pulp-test-renderer3d --reporter compact
```

Current result:

- Build passed.
- Test passed on Metal: 47 assertions in 2 test cases.
- Link line contains `pulp-render`, `pulp-canvas`, `libskia.a`, and
  `libwgpu_native.dylib`; it does not contain `pulp-view`, V8, QuickJS,
  JavaScriptCore, Three.js, or web-compat assets.
- Proof PNG was written to the macOS temp directory as
  `pulp-renderer3d-hardcoded-cube.png` and visually inspected as a rotated
  textured cube. The generated-GLB path also writes
  `pulp-renderer3d-scenedata.png` and
  `pulp-renderer3d-box-textured-like.png`; these are inspected as visible
  textured geometry. The official fixture path writes
  `pulp-renderer3d-box-textured-official.png` for inspection.

A no-Skia cache also built `pulp-test-renderer3d`; it soft-skips the render
proof when Dawn/Skia is unavailable rather than failing headless CI.

## Native Loader Boundary And Dependency Decision

The native loader remains behind `core/scene/include/pulp/scene/gltf_loader.hpp`.
No fastgltf, cgltf, tinygltf, Draco, Dawn, Skia, or Three.js type appears in the
public scene headers. This keeps the loader swappable and lets the renderer
consume only engine-owned CPU data.

Dependency decision, rechecked during this slice:

- Primary loader: fastgltf. Current upstream tag observed from GitHub tags:
  `v0.9.0` (`0d1b67a28c4950ea2deb796702006dcbe31e02b3`). It remains the best
  fit for the planned implementation because it is MIT, active, C++17, fast,
  and exceptions-off friendly, while keeping Draco decode separate.
- Fallback loader: cgltf. Current upstream tag observed from GitHub tags:
  `v1.15` (`360db1a95480fe102ae9c69b27c5d101167ff5ba`). It remains the
  low-dependency fallback if simdjson/fastgltf cost becomes unacceptable.
- Rejected for this lane: tinygltf. Current upstream tag observed from GitHub
  tags: `v3.0.0` (`135695e91898081e966ab4588938065ace765462`). It is still a
  reasonable tool, but its header-only dependency shape and optional embedded
  decode choices make it a worse match for Pulp's thin loader boundary.

Dependency/CMake/docs/notice edits were coordinated through #2738 before
implementation:
https://github.com/Generous-Corp/pulp/issues/2738#issuecomment-4609687265

The opt-in dependency graph now adds fastgltf `v0.9.0` and explicit simdjson
`v3.12.3` FetchContent entries only when `PULP_ENABLE_SCENE3D=ON`. Explicit
simdjson keeps fastgltf's transitive parser dependency visible to Pulp's
dependency audit instead of letting fastgltf download it internally. The
dependency inventory was updated in `tools/deps/manifest.json`,
`DEPENDENCIES.md`, `NOTICE.md`, and `docs/reference/licensing.md`.

## Native GLB Loader Slice

The first native GLB loader slice is implemented in
`core/scene/src/gltf_loader.cpp` with fastgltf kept private to the `.cpp`.
`scene3d-gltf-loader-surface-contract` statically pins that boundary: the
public `gltf_loader.hpp` API exposes only `SceneData`, `LoadOptions`, and the
engine-owned Draco callback data; the source keeps fastgltf includes private,
preserves the ordered load pipeline, and keeps loader diagnostic codes stable.
`scene3d-gltf-loader-surface-negative-contract` mutates temporary copies to
prove the verifier rejects parser leakage, callback-signature drift, pipeline
drift, diagnostic-code drift, Draco-flow drift, and parser-extension drift.
The parser extension mask now explicitly enables
`KHR_materials_emissive_strength` alongside the rest of the native material
floor so `emissiveStrength` survives fastgltf parsing into `SceneData`.
It supports the current native floor:

- GLB parsing with embedded buffer payloads.
- Triangle primitives with POSITION, NORMAL, TANGENT, TEXCOORD_0, TEXCOORD_1,
  COLOR_0, and unsigned byte/short/int indices.
- Node roots plus TRS and matrix transforms.
- Material names, double-sided flags, unlit extension presence,
  base-color factor, and base-color texture references.
- Metallic/roughness factors, metallic-roughness textures, normal textures,
  occlusion textures, emissive textures/factors, alpha mode, and alpha cutoff
  are preserved in CPU `SceneData`.
- Normal texture scale and occlusion texture strength are preserved as
  material scalar data and applied by the current default-path native texture
  sampling proof when the corresponding textures are sampled.
- Texture coordinate set indices are preserved for base-color,
  metallic-roughness, normal, occlusion, and emissive texture slots.
  `KHR_texture_transform` offset, scale, rotation, and texCoord overrides are
  preserved for those slots when present; the current native renderer does not
  apply them yet.
- glTF sampler state is normalized into `TextureSamplerData` records. Material
  texture slots preserve sampler indices so future native texture upload can
  apply filter and wrap modes without reparsing glTF.
- Advanced physical-material extensions such as clearcoat, transmission,
  sheen, specular, volume, anisotropy, iridescence, diffuse transmission, IOR,
  and dispersion are recorded as material extension names for sidecar
  diagnostics and produce loader warnings. They are not included in the current
  native renderer material floor.
- Perspective and orthographic cameras, with node camera references.
- `KHR_lights_punctual` directional, point, and spot lights, with node light
  references.
- TRS animation channels for translation, rotation, and scale, with keyframe
  sampler input/output arrays stored in CPU `SceneData`.
- Embedded image bytes from buffer views, arrays, vectors, and byte views.
- Local external `.gltf` buffer and image files resolved relative to the source
  glTF path.
- Diagnostics for malformed files, unsupported primitive modes, missing
  accessors, unsupported animation paths, advanced material extensions, and
  Draco primitives. Draco diagnostics distinguish the no-decoder native wiring
  gap, decode failures, invalid Draco buffer views, and
  `LoadOptions::allow_draco=false`.
- Unsupported native mesh features are surfaced through both loader diagnostics
  and sidecar unsupported-feature records: morph targets/default weights,
  skinning, and `EXT_mesh_gpu_instancing`. Node morph weights remain gated by
  fastgltf v0.9.0 parse behavior, which rejects the node `weights` assets before
  loader-side diagnostics can run.

Known limitations:

- Draco compressed primitives populate `SceneData` when a native Draco decode
  callback is supplied. The loader maps fastgltf's compressed bufferView and
  POSITION, NORMAL, TEXCOORD_0, TEXCOORD_1, TANGENT, and COLOR_0 unique IDs
  into the callback result. The render-side Draco scene adapter now supplies
  that callback when callers opt in; the default loader still reports
  `gltf.draco_not_wired` if no callback is supplied. The native inspector has a
  hermetic invalid-Draco smoke that runs both paths: default inspect must report
  `gltf.draco_not_wired`, while `--native-draco` must report decoder
  availability plus either `gltf.draco_unavailable` or `gltf.draco_decode_failed`
  without falling back to the not-wired diagnostic. In the default Draco-off
  build it emits `gltf.draco_unavailable` plus decoder availability status; a
  Draco-on build would continue to report
  `gltf.draco_decode_failed` for the intentionally invalid payload. The
  `scene3d-inspect-native-draco-diagnostic-negative-contract` test feeds fake
  inspectors through that same smoke so it must reject default-path exit drift,
  missing `gltf.draco_not_wired`, missing or ambiguous native availability,
  wrong native diagnostics for enabled/disabled decoder states, and a native
  callback path that still leaks `gltf.draco_not_wired`. A Draco-on unit test
  source-generates a compressed mesh with Draco's
  `TriangleSoupMeshBuilder` and encoder, then decodes through Pulp's
  unique-id path and asserts POSITION, NORMAL, TEXCOORD_0, TEXCOORD_1, TANGENT,
  COLOR_0, and index parity without vendoring a compressed binary fixture. A
  checked-in compressed GLB fixture parity test remains outstanding.
- No animation playback, skinning, morph target evaluation, GPU instancing, or
  GPU animation update path yet; these remain live/deferred.
- The shared `BoxTextured.glb` fixture now exists as a checked-in native test
  input with Khronos/Cesium attribution in the fixture README, `NOTICE.md`, and
  `docs/reference/licensing.md`.

## Native SceneData Render Join

`Renderer3D::render_scene_data()` now proves the loader-to-renderer join without
JS, Three.js, web-compat, or fastgltf types crossing the public renderer
boundary. The public render header forward-declares `SceneData`; `pulp-render`
uses the scene headers privately and the renderer test links `pulp::scene`
because the test calls `load_gltf_scene()`.

Current render floor:

- Validate `SceneData` structurally before creating GPU resources.
- Walk the `SceneData` node graph from scene roots, including child nodes.
- Compose node translation, rotation, scale, and matrix transforms into CPU
  world transforms, then normalize collected renderable primitives into the
  offscreen view volume.
- Render multiple parsed glTF root nodes that reference the same mesh as
  separate `SceneData` render primitives.
- Draw multiple renderable primitives in one offscreen pass, with per-primitive
  vertex/index buffers, uniforms, texture/sampler bindings, cull state, and
  alpha-blend state for the current base-color shader path.
- Upload SceneData-derived vertices, uint32 indices, uniforms, and texture data
  for each rendered primitive.
- Decode embedded image bytes through Skia when possible.
- Use a deterministic material-colored fallback texture when image decode is
  unavailable or the encoded payload is intentionally minimal.
- Upload base-color texture data as `rgba8unorm-srgb` so shader sampling
  follows glTF's sRGB base-color texture convention on the current path.
- Apply preserved base-color texture sampler state for the current SceneData
  draw path, mapping glTF wrap and nearest/linear filter modes into the WebGPU
  sampler descriptor. Mipmapped min filters are detected and downgraded to a
  single-level texture sampler path until native mip generation exists.
- Apply preserved base-color texture coordinate selection and
  `KHR_texture_transform` metadata on the CPU before vertex upload, including a
  TEXCOORD_1 override when the primitive provides secondary UVs.
- Apply preserved base-color factors as shader uniforms for the base-color
  texture/fallback path.
- Apply preserved metallic and roughness scalar factors as a bounded material
  energy term on the current lit base-color path.
- Apply preserved `KHR_materials_unlit` / material unlit state by bypassing the
  current depth-based shade term for the base-color texture/fallback path.
- Apply preserved `alphaMode=MASK` and `alphaCutoff` by discarding base-color
  samples whose combined texture/factor alpha falls below the cutoff.
- Apply preserved `COLOR_0` vertex colors by multiplying them into the
  base-color texture/fallback path, including vertex alpha in the existing
  alpha-mask decision.
- Apply preserved double-sided material state by disabling back-face culling for
  double-sided primitives and culling back faces for single-sided primitives.
- Apply preserved `alphaMode=BLEND` by enabling standard source-alpha blending
  for blended materials on the current base-color path, submitting opaque
  primitives before blended primitives, and disabling depth writes for blended
  pipelines.
- Apply preserved emissive factors and emissive strength as an additive term on
  the current base-color path.
- Decode, upload, and sample preserved emissive texture bytes as sRGB color
  data when the material uses the default emissive TEXCOORD_0 path, multiplying
  the sampled texture color by emissive factor and strength.
- Decode, upload, and sample preserved metallic-roughness texture bytes as
  linear data when the material uses the default metallic-roughness TEXCOORD_0
  path, multiplying roughness by the texture G channel and metallic by the B
  channel in the current material-energy approximation.
- Decode, upload, and sample preserved occlusion texture bytes as linear data
  when the material uses the default occlusion TEXCOORD_0 path, darkening the
  lit term with the texture R channel and preserved occlusion strength.
- Decode, upload, and sample preserved normal texture bytes as linear data when
  the primitive has tangents and the material uses the default normal TEXCOORD_0
  path, reconstructing a basic tangent-space normal for the current lighting
  terms with preserved normal scale.
- Apply preserved geometry `NORMAL` attributes as a basic directional-lighting
  term on the current base-color path.
- Apply the first preserved directional light's color and intensity to the
  current lit base-color path.
- Apply the first preserved directional light node transform as the light
  direction for the current normal-lit path.
- Apply the first preserved point light's color, intensity, and node position
  as an additive light term on the current base-color path.
- Apply the first preserved spot light's color, intensity, node position, node
  direction, and cone angles as an additive cone light term on the current
  base-color path.
- Apply preserved point and spot light `range` as a simple distance attenuation
  term on the current additive punctual-light path.
- Apply the first preserved perspective camera's `yfov` as a narrow
  projection-scale hint on the current vertex path.
- Apply the first preserved orthographic camera's `ymag` as an orthographic
  projection-scale hint on the current vertex path.
- Apply preserved camera `aspectRatio` as the horizontal scale for the current
  camera projection path.
- Apply preserved camera `znear` / `zfar` as a bounded depth remap for the
  current simplified camera projection path.
- Apply preserved camera node translation as a normalized view offset for the
  current camera projection path.
- Apply preserved camera node rotation as a normalized view basis for the
  current camera projection path.
- Apply preserved rigid camera node matrices, using their translation and
  orthonormal basis for the current camera projection path.
- Apply the first preserved TRS animation key as the renderer's initial static
  pose for translation, rotation, and scale channels.
- Render to an owned offscreen color target and depth target, read RGBA back,
  and PNG-encode the result for inspection.

This renderer still uses a proof shader centered on the base-color
texture/fallback path, with emissive, metallic-roughness, occlusion, and normal
texture sampling now wired as the first non-base-color texture slots.
Base-color and emissive texture bytes are uploaded as sRGB textures;
metallic-roughness, occlusion, and normal texture bytes are uploaded as linear
`RGBA8Unorm` data.
Render-target/presentation color management remains deferred. Base-color
factors, sampler state, base-color texture transform metadata, unlit material
state, alpha-mask cutoff, vertex colors, double-sided material state,
metallic/roughness scalar factors, metallic-roughness texture factors,
occlusion texture strength, and tangent-space normal textures are now consumed
for that path when their texture coordinate routes are available.
Mipmapped sampler requests are reported and downgraded because textures are
uploaded with one mip level in this proof renderer. Non-base-color material
texture slots now carry separate UV attributes, so the proof renderer consumes
their TEXCOORD_1 and `KHR_texture_transform` routes when the primitive provides
the requested coordinate set. Alpha-blend state is
consumed by sorting opaque draws before blended draws and by using a
no-depth-write pipeline for blended primitives on the current base-color target.
Multiple blended primitives are sorted back-to-front by the current normalized
primitive depth; full per-triangle/per-pixel transparent sorting remains
deferred. Emissive factors/strength are
consumed as an additive term, geometry normals affect the current basic lighting
term, the first directional light can tint lit materials and derive its
direction from a preserved light node transform, and the first
perspective camera can scale projection from its preserved `yfov` and
`aspectRatio`, and camera `znear` / `zfar` metadata is consumed as a bounded
depth remap for the current simplified camera path. Camera node translation is
consumed as a view offset in the current normalized scene space, and camera node
rotation is consumed as a view basis for that proof path. Rigid camera node
matrices are also consumed when their basis is orthonormal. The first
orthographic camera can scale
projection from its preserved `ymag` without a perspective divide. Real mip
generation, texture coordinate sets beyond TEXCOORD_1, camera node
scale/shear/perspective matrix transforms, shadows, and full view/projection
matrices are not wired yet. Camera scale/shear/perspective matrix transforms are
detected and reported as deferred when present, so the renderer does not
silently ignore preserved view data.
Point and spot light attenuation ranges are consumed by the current base-color
shader for the first preserved point and spot lights; additional or unattached
range-bearing punctual lights are still reported as deferred so they are not
silently ignored. Spot light records and cone metadata are also consumed by the
current base-color shader.
The first preserved TRS animation key is consumed as an initial static pose;
playback, time sampling, and GPU animation updates are still reported as
deferred.
Loader-preserved unsupported feature records for skinning, morph targets/weights,
and GPU instancing are surfaced as renderer deferrals while the static mesh path
continues to render.
Advanced physical-material extensions are reported as deferred when preserved on
a material; clearcoat/transmission/etc. are not sampled by this proof shader.
The remaining PBR material fields
are preserved and keyed for later pipeline-cache work. The metallic/roughness
scalar and texture path is intentionally a small material-energy input, not full
specular BRDF or IBL; the occlusion path darkens the lit term without
ambient/IBL separation; and the normal-map path is a basic tangent-space normal
input to the existing lighting terms, not a validated glTF-Sample-Renderer
BRDF. The renderer now reports when a primitive carries authored tangent
attributes, when it derives a tangent basis for a normal texture from indexed
triangle positions/normals/UVs, and when a material carries preserved
metallic-roughness, normal, occlusion, or emissive texture slots.
Preserved normal-scale and occlusion-strength modifiers are also reported as
deferred when their texture slots are present but cannot be applied. Preserved
texture transforms and TEXCOORD_1 routing for those non-base-color slots are
consumed when the primitive provides the requested coordinate set. Higher
texture coordinate sets remain reported as deferred. Metallic-roughness,
normal, occlusion, and emissive textures are decoded, uploaded, and sampled
when the texture is readable and the texture coordinate route is supported;
normal textures additionally require either authored tangents or a derivable
tangent basis. Degenerate normal UV routes remain deferred.
Multiple renderable primitives are collected and drawn with per-primitive
base-color material uniforms and render state. The SceneData renderer now keeps
an in-call pipeline cache for the current render-state variants (`alphaMode`
blend and double-sided culling), so material uniform changes can reuse a native
pipeline. This is still an immediate-mode proof path that creates per-primitive
buffers, textures, samplers, uniforms, and bind groups during the call; a
persistent renderer-level pipeline/material cache remains deferred before
broader assets are claimed.
Tangent attributes are preserved and keyed for the normal-map pipeline, and the
renderer reports authored and derived tangent availability alongside deferred
PBR texture slots.
Secondary UVs are also preserved and keyed for later occlusion material variants;
vertex colors are consumed by the current base-color render path.

The generated renderer test GLB carries positions, TEXCOORD_0, uint16 indices,
a material, a base-color sampler, a valid embedded PNG image buffer view, and a
translated node. The loader maps it into `SceneData`; the renderer consumes that
parsed data, decodes the embedded PNG through Skia instead of using the fallback
texture, and writes `pulp-renderer3d-scenedata.png` in the macOS temp directory.
The inspected PNG is a visible textured triangle. The renderer test now checks
foreground regions against the clear color and pins image decode, sRGB
base-color texture upload, plus sampler consumption, so a fully opaque but blank
clear pass, ignored image payload, or ignored base-color sampler cannot satisfy
the Skia-backed assertions. A generated external glTF smoke writes separate
`mesh.bin` and `checker.png` temp files, loads them through fastgltf into
`SceneData`, then renders that decoded external image path without adding or
mutating a shared fixture. The official `BoxTextured.glb` fixture now separately
pins the real Khronos sample asset path through the same native renderer. A separate
renderer smoke covers a mesh under a child node, preventing regression to
root-node-only rendering. Another direct `SceneData` renderer smoke pins
base-color texture transform consumption and TEXCOORD_1 override handling
without adding a shared fixture. A mipmapped-sampler smoke verifies that
linear mipmap minification requests are accepted as sampler metadata, preserve
linear minification, and report the single-level mipmap downgrade. A
geometry-normal smoke renders the same triangle with opposing normals and
verifies the simple native lighting term changes whole-image luminance under
the sRGB base-color path. A multi-primitive smoke renders two
same-state primitives from one `SceneData` mesh and asserts the renderer reports
both primitives instead of stopping after primitive zero, with one pipeline
cache entry and one cache hit. A mixed-material primitive smoke renders red and
green primitives in the same mesh, verifying that each primitive receives its
own base-color material uniforms while sharing the same render-state pipeline.
An emissive-strength smoke renders the same emissive factor with default and
boosted preserved strength, asserts `emissive_strength_applied`, and verifies
the boosted render raises foreground blue.
An emissive-texture smoke renders a low-base-color triangle with a decoded
1x1 blue emissive PNG, asserts `emissive_texture_applied` without
`emissive_texture_deferred`, and verifies the foreground blue channel dominates
red and green. A transformed emissive-texture smoke routes that slot through
TEXCOORD_1 plus a preserved offset transform and verifies the blue emissive
column is sampled without non-base texture-transform or TEXCOORD_1 deferral.
An alpha-over-opaque smoke puts a red blended primitive before a green opaque
primitive in `SceneData`, then verifies the renderer reports the no-depth-write
blend path and the foreground contains both red and green after opaque-before-
blend submission. An alpha-sort smoke submits a front red blended primitive
before a back blue blended primitive, then verifies the renderer reports
`alpha_blend_sorted` and the front layer dominates after back-to-front
primitive ordering. A directional light smoke renders a white material under a
green key light and verifies the foreground shifts green while
`directional_light_applied` is reported. A directional-light transform smoke
renders the same normal-lit triangle with identity and rotated directional
light nodes, then verifies `directional_light_transform_applied` plus a
measurable luminance change. A punctual-light smoke renders
a material with preserved point and spot lights and verifies
`point_light_applied` with a red foreground shift and `spot_light_applied` with
a blue foreground shift while `punctual_light_range_applied` confirms that
preserved distance falloff metadata is consumed for attached point and spot
lights; the same test keeps range deferral pinned for missing/unattached
punctual nodes. A separate point-light range smoke compares short and long
ranges and verifies the long range produces a stronger foreground red channel.
`punctual_light_range_deferred`, `spot_light_deferred`,
`spot_light_cone_deferred`, and `directional_light_applied` stay false for the
fully attached point+spot fixture. A perspective-camera smoke renders the
same triangle with and without a preserved camera node, then verifies the
foreground grows while
`perspective_camera_applied` is reported. A camera translation smoke renders a
translated camera node and verifies `camera_node_translation_applied` plus a
measurable foreground shift while pure translation is not reported as a deferred
camera transform. A camera aspect-ratio smoke renders the same camera with two
preserved aspect ratios and verifies the foreground width changes while
`camera_aspect_ratio_applied` is reported. A camera/light transform deferral
smoke renders rotated camera and directional-light nodes and verifies
`camera_node_rotation_applied` and `directional_light_transform_applied` are
reported with GPU submit/readback complete while rotation-only camera nodes are
not reported as deferred. A camera rotation smoke compares translated and rotated camera nodes, then
verifies `camera_node_rotation_applied` plus a measurable foreground shift
while rotation-only camera transforms are not reported as deferred. A camera
matrix smoke compares translated and rotated rigid camera matrices, then
verifies the same camera translation/rotation flags plus a measurable foreground
shift while rigid camera matrices are not reported as deferred. A camera
metadata smoke renders a perspective camera carrying preserved aspect ratio and
depth range, then verifies `camera_aspect_ratio_applied` and
`camera_depth_range_applied` are reported without camera metadata deferrals. An
orthographic-camera smoke renders the same triangle with two preserved `ymag`
values, then verifies the smaller orthographic extent grows the foreground while
`orthographic_camera_applied` is reported and `perspective_camera_applied` stays
false. Transform-animation smokes compare static meshes with SceneData meshes
carrying preserved translation, cubic-spline rotation, and scale animation
channels, verify the first key changes the rendered framebuffer, and report both
`transform_animation_initial_pose_applied` and `transform_animation_deferred`
because playback remains deferred. An
advanced-material deferral smoke renders a material with
preserved clearcoat/transmission extension names and verifies
`advanced_material_extension_deferred` is reported while the base-color fallback
path still renders. An unsupported-feature deferral smoke renders a static
triangle carrying preserved loader unsupported-feature records for skinning,
morph targets, and GPU instancing, then verifies `skinning_deferred`,
`morph_target_deferred`, and `gpu_instancing_deferred` are reported while the
base-color fallback
path still renders. A normal-map deferral smoke renders a
primitive with normals, tangents, and an unreadable normal texture slot, then
asserts the geometry-normal path still renders while
`tangent_attributes_available`, `normal_texture_deferred`, and
`normal_scale_deferred` report that the normal-map inputs are present but not
sampled. A normal-map sampling smoke renders a tangent-bearing triangle with a
decoded 1x1 reverse-Z normal PNG, asserts `normal_texture_applied` without
`normal_texture_deferred` or `normal_scale_deferred`, and verifies the sampled
normal darkens the rendered foreground relative to the geometry normal path.
A normal-scale smoke renders the same decoded normal map with default and
boosted preserved `normalScale`, asserts `normal_scale_applied` for the
non-default value, and verifies the boosted scale changes the rendered
framebuffer.
A derived-tangent normal-map smoke clears the authored tangents on that same
triangle and verifies `tangent_attributes_derived`, `normal_texture_applied`,
and the same foreground darkening.
`tools/scene3d/renderer_probe_derived_normal_route_smoke.py`, wired through
`scene3d-renderer-probe-derived-normal-route-contract`, now pins the same
derived-tangent normal-map floor through the command-line probe with a
generated temp glTF: the primitive provides normals and UVs but no authored
tangents, and the probe must report derived tangent availability, normal-map
sampling, non-default normal-scale application, no normal deferrals, and
positive pixel output. Its negative companion,
`scene3d-renderer-probe-derived-normal-route-negative-contract`, feeds fake
probe output through the same smoke and verifies stale telemetry is rejected for
missing derived tangents, lost normal texture or normal-scale application,
leaked normal deferrals, and missing pixel output.
`tools/scene3d/renderer_probe_alpha_blend_sort_route_smoke.py`, wired through
`scene3d-renderer-probe-alpha-blend-sort-route-contract`, generates a temp glTF
with two alpha-blended primitives submitted front-first and verifies the probe
reports two primitives, alpha blending, disabled depth writes for blended
draws, back-to-front blend sorting, and positive pixel output. Its negative
companion rejects primitive-count drift, lost blend/depth-write/sort telemetry,
unexpected alpha-mask leakage, and missing pixel output.
`tools/scene3d/renderer_probe_alpha_mask_route_smoke.py`, wired through
`scene3d-renderer-probe-alpha-mask-route-contract`, generates a temp glTF with
an embedded alpha PNG and `alphaMode: MASK`, then verifies the probe reports
texture decode/upload, base-color sRGB sampling, alpha-mask application, no
alpha-blend leakage, no fallback texture, and positive pixel output. Its
negative companion rejects lost alpha-mask, texture decode, or sRGB telemetry,
unexpected blend/fallback leakage, and missing pixel output.
`tools/scene3d/renderer_probe_vertex_color_route_smoke.py`, wired through
`scene3d-renderer-probe-vertex-color-route-contract`, generates a temp glTF
with a float `COLOR_0` accessor and verifies the probe reports vertex-color
application, the current fallback texture route, no decoded texture, no
alpha-state leakage, and positive pixel output. Its negative companion rejects
lost vertex-color telemetry, fallback-route drift, texture-decode or alpha-mask
leakage, primitive-count drift, and missing pixel output.
`tools/scene3d/renderer_probe_double_sided_route_smoke.py`, wired through
`scene3d-renderer-probe-double-sided-route-contract`, generates a temp glTF
with a back-facing triangle and `doubleSided: true`, then verifies the probe
reports double-sided material consumption, the current fallback texture route,
no unrelated vertex-color or alpha state, and positive pixel output. Its
negative companion rejects lost double-sided telemetry, fallback-route drift,
unexpected texture-decode or alpha-blend leakage, primitive-count drift, and
missing pixel output.
`tools/scene3d/renderer_probe_unlit_route_smoke.py`, wired through
`scene3d-renderer-probe-unlit-route-contract`, generates a temp glTF with
`extensionsUsed: ["KHR_materials_unlit"]` and a material extension payload, then
verifies the probe reports unlit material consumption, the current fallback
texture route, no decoded texture, no double-sided or alpha-state leakage, and
positive pixel output. Its negative companion rejects lost unlit telemetry,
fallback-route drift, texture-decode leakage, unexpected double-sided or
alpha-blend leakage, primitive-count drift, and missing pixel output.
`tools/scene3d/renderer_probe_emissive_route_smoke.py`, wired through
`scene3d-renderer-probe-emissive-route-contract`, generates a temp glTF with
`emissiveFactor` plus `KHR_materials_emissive_strength`, then verifies the probe
reports emissive factor and strength consumption, the current fallback texture
route, no decoded texture, no emissive texture route, and no unrelated unlit,
double-sided, vertex-color, or alpha-state leakage. Its negative companion
rejects lost emissive-factor or emissive-strength telemetry, fallback-route
drift, texture-decode or emissive-texture leakage, unlit leakage,
primitive-count drift, and missing pixel output.
`tools/scene3d/renderer_probe_sampler_route_smoke.py`, wired through
`scene3d-renderer-probe-sampler-route-contract`, generates a temp glTF with an
embedded PNG base-color texture and a preserved linear clamp sampler, then
verifies the probe reports texture decode/upload, base-color sRGB sampling,
sampler application, clamp-S/clamp-T/linear sampler telemetry, no fallback or
unrelated material-route leakage, and positive pixel output. Its negative
companion rejects lost sampler/clamp/linear telemetry, texture-decode drift,
fallback leakage, primitive-count drift, and missing pixel output.
`tools/scene3d/renderer_probe_mipmap_sampler_route_smoke.py`, wired through
`scene3d-renderer-probe-mipmap-sampler-route-contract`, uses the same generated
embedded-PNG route with a `LINEAR_MIPMAP_LINEAR` min filter, then verifies the
probe reports sampler application, linear filtering, the current single-level
`texture_mipmap_filter_downgraded` path, no fallback leakage, and positive pixel
output. Its negative companion rejects lost mipmap-downgrade, sampler, linear,
or texture-decode telemetry, fallback leakage, primitive-count drift, and
missing pixel output.
`tools/scene3d/renderer_probe_base_color_transform_route_smoke.py`, wired
through `scene3d-renderer-probe-base-color-transform-route-contract`, generates
a temp glTF with an embedded PNG base-color texture plus `KHR_texture_transform`
metadata that overrides the texture route to `TEXCOORD_1`, then verifies the
probe reports texture decode/upload, base-color sRGB sampling, base-color
transform application, base-color TEXCOORD_1 routing, no fallback or non-base
texture-transform leakage, and positive pixel output. Its negative companion
rejects lost transform or TEXCOORD_1 telemetry, texture-decode drift, fallback
leakage, unrelated non-base transform leakage, primitive-count drift, and
missing pixel output.
`tools/scene3d/renderer_probe_directional_light_route_smoke.py`, wired through
`scene3d-renderer-probe-directional-light-route-contract`, generates a temp
glTF with `KHR_lights_punctual`, an attached directional light, and authored
normals, then verifies the probe reports directional-light and geometry-normal
consumption, the current fallback texture route, no point/spot light leakage,
no light-transform deferral, and positive pixel output. Its negative companion
rejects lost directional-light or normal telemetry, unexpected directional
transform/point/spot leakage, primitive-count drift, and missing pixel output.
`tools/scene3d/renderer_probe_point_light_route_smoke.py`, wired through
`scene3d-renderer-probe-point-light-route-contract`, generates a temp glTF with
`KHR_lights_punctual`, an attached point light carrying range metadata, and a
translated light node, then verifies the probe reports point-light application,
range consumption, no point/range deferral, no directional or spot leakage, and
positive pixel output. Its negative companion rejects lost point-light or range
telemetry, leaked point/range deferral, unexpected directional/spot leakage,
primitive-count drift, and missing pixel output.
`tools/scene3d/renderer_probe_spot_light_route_smoke.py`, wired through
`scene3d-renderer-probe-spot-light-route-contract`, generates a temp glTF with
`KHR_lights_punctual`, an attached spot light carrying range and cone metadata,
and a translated light node, then verifies the probe reports spot-light
application, range consumption, no spot/range/cone deferral, no directional or
point leakage, and positive pixel output. Its negative companion rejects lost
spot-light or range telemetry, leaked spot/range/cone deferral, unexpected
directional/point leakage, primitive-count drift, and missing pixel output.
`tools/scene3d/renderer_probe_perspective_camera_route_smoke.py`, wired through
`scene3d-renderer-probe-perspective-camera-route-contract`, generates a temp
glTF with a perspective camera node carrying translation, aspect ratio, and
depth-range metadata, then verifies the probe reports perspective-camera
application, camera translation, aspect-ratio consumption, depth-range
consumption, no orthographic or camera-deferral leakage, and positive pixel
output. Its negative companion rejects lost perspective/translation/aspect/depth
telemetry, unexpected orthographic or deferred-camera leakage, primitive-count
drift, and missing pixel output.
`tools/scene3d/renderer_probe_orthographic_camera_route_smoke.py`, wired
through `scene3d-renderer-probe-orthographic-camera-route-contract`, generates
a temp glTF with an orthographic camera node carrying preserved x/y magnitudes
and depth-range metadata, then verifies the probe reports orthographic-camera
application, depth-range consumption, no perspective/aspect/transform or
camera-deferral leakage, and positive pixel output. Its negative companion
rejects lost orthographic/depth telemetry, unexpected perspective-camera or
deferred-camera leakage, primitive-count drift, and missing pixel output.
A broader PBR texture-slot deferral smoke
renders a material with preserved metallic-roughness, normal, occlusion, and
emissive texture slots and a degenerate normal UV route, then verifies the
renderer reports only the remaining normal-texture/normal-scale deferral while
still applying the readable metallic-roughness, occlusion, and emissive texture
routes. A metallic/roughness factor smoke renders the same
material with smooth dielectric versus rough metallic scalars and verifies
`metallic_roughness_factor_applied` plus a measurable luma change on the current
base-color path. A metallic-roughness texture smoke renders rough metallic
scalars with a decoded 1x1 black metallic-roughness PNG, asserts
`metallic_roughness_texture_applied` without
`metallic_roughness_texture_deferred`, and verifies the texture's G/B channels
increase foreground luma by lowering roughness and metallic inputs in the
current material-energy approximation. A transformed metallic-roughness smoke
routes that slot through TEXCOORD_1 plus a preserved offset transform, verifies
`metallic_roughness_texture_applied` plus the positive
`non_base_color_texture_transform_applied` and
`non_base_color_texcoord1_used` telemetry without non-base transform/TEXCOORD_1
deferral, and checks that sampling the shifted blue column changes the
rendered framebuffer and material-energy luma relative to the unshifted black
column. `tools/scene3d/renderer_probe_non_base_texture_route_smoke.py`, wired
through `scene3d-renderer-probe-non-base-texture-route-contract`, now pins the
same positive route at the command-line probe boundary with a generated temp
glTF fixture: metallic-roughness, occlusion, and emissive textures all route
through `TEXCOORD_1` plus `KHR_texture_transform`, report applied texture and
occlusion-strength telemetry, and reject the corresponding non-base texture
route deferral flags. Its companion
`scene3d-renderer-probe-non-base-texture-route-negative-contract` feeds fake
probe output to the smoke and verifies drift is rejected when applied route
fields disappear, texture-route telemetry flips false, stale deferral flags
leak back in, or pixel output is no longer reported.
An occlusion texture smoke renders a smooth dielectric material with a decoded
1x1 black occlusion PNG, asserts `occlusion_texture_applied` without
`occlusion_texture_deferred` or `occlusion_strength_deferred`, and verifies the
foreground luma drops relative to the same material without occlusion. An
occlusion-strength smoke renders the same occlusion texture with weak and full
preserved strength, asserts `occlusion_strength_applied` for the non-default
strength, and verifies the full-strength render darkens the foreground further.
A transformed occlusion smoke routes that slot through TEXCOORD_1 plus a
preserved offset transform, verifies the positive non-base transform/TEXCOORD_1
telemetry and that the route is no longer deferred, and checks that sampling
the shifted white column brightens the foreground relative to the unshifted
black occlusion column.

## Scene Graph Slice

`core/scene/include/pulp/scene/scene_graph.hpp` now owns the CPU transform
boundary the native renderer consumes:

- `local_transform_for_node()` turns node TRS into a column-major transform.
- Matrix-form glTF node transforms are preserved in `SceneData` and preferred
  over TRS during traversal.
- `collect_renderable_nodes()` traverses scene roots, composes parent/child
  world transforms, and returns node/mesh pairs.
- Cycles produce explicit `scene.graph_cycle` diagnostics instead of recursing
  indefinitely.
- `scene3d-scene-graph-surface-contract` statically pins the same CPU handoff
  surface: `Mat4` identity shape, renderable/transformed node fields, public
  traversal declarations, matrix-vs-TRS selection, quaternion normalization,
  column-major matrix multiply / point transform, root fallback traversal, and
  graph diagnostic codes.
- `scene3d-scene-graph-surface-negative-contract` feeds temporary
  `scene_graph` header/source mutations through that verifier so it must reject
  identity initializer drift, renderable field drift, public declaration drift,
  cycle diagnostic drift, world-multiply order drift, child traversal drift, root
  fallback drift, matrix-copy drift, quaternion-normalization drift, and point
  translation drift.

This remains CPU-only and fastgltf-free. It moves the renderer toward the Phase
3 scene/runtime abstraction without adding GPU-resource ownership or touching
runtime-owned `core/view`.

## Render Packet Slice

`core/scene/include/pulp/scene/render_packet.hpp` now owns the CPU packet the
native renderer consumes after parsing:

- `build_render_packet()` runs structural validation, composes node world
  transforms, and stops before producing primitives if validation or graph
  traversal reports errors.
- `RenderPrimitive` binds node index, mesh index, primitive index, world
  transform, and `MaterialKey` in one renderer-facing record.
- Empty/non-indexed primitives are filtered before the GPU path; an otherwise
  populated scene with no renderable primitive gets a
  `scene.render_packet_empty` diagnostic.

The packet deliberately contains no decoded textures, buffers, Dawn handles, or
pipeline objects. Those remain renderer-owned so `core/scene` stays CPU-only
and the loader-to-renderer boundary stays swappable.

## Scene Stats Slice

`core/scene/include/pulp/scene/scene_stats.hpp` now provides a deterministic
loader-inspection harness for parsed `SceneData`:

- `summarize_scene_data()` counts nodes, roots, meshes, primitives, indexed
  primitives, vertices, indices, materials, textures, texture samplers, encoded
  texture bytes, advanced material extensions, cameras, lights, animations,
  unsupported features, diagnostics, and error diagnostics.
- `scene_stats_to_text()` produces a stable one-line summary suitable for a
  future CLI `pulp ... inspect` / bake preflight command without introducing
  CLI coupling in this native spike.
- Tests pin both an in-memory scene summary and the official
  `BoxTextured.glb` fixture summary after fastgltf loading.
- `scene3d-scene-stats-surface-contract` statically pins the stats struct
  fields, aggregation sources, and `scene_stats_to_text()` key order. The
  fixture checks prove representative values; this source-level guard prevents
  inspect/preflight summary drift before a new fixture exposes it.
- `scene3d-scene-stats-surface-negative-contract` feeds temporary header/source
  mutations through that verifier so it must reject missing or reordered stats
  fields, text key drift, primitive/vertex aggregation drift, encoded texture
  byte drift, advanced material extension count drift, and error diagnostic
  count drift.

This gives Phase 1 the requested parsed-scene stats harness while keeping the
module CPU-only and free of fastgltf types at the public boundary.

## Scene Stats Harness Slice

`core/scene` now builds `pulp-scene3d-inspect` whenever
`PULP_ENABLE_SCENE3D=ON`. The executable is intentionally outside the main
user-facing `pulp` CLI so the native 3D spike remains opt-in and does not pull
fastgltf into default CLI builds.

Usage:

```bash
./build-scene3d/core/scene/pulp-scene3d-inspect \
  test/fixtures/scene3d/BoxTextured/BoxTextured.glb
```

Expected first-slice output for the official fixture:

```text
nodes=2 roots=1 meshes=1 primitives=1 indexed_primitives=1 vertices=24 indices=36 materials=1 textures=1 texture_samplers=1 texture_bytes=3750 advanced_material_extensions=0 cameras=0 lights=0 animations=0 unsupported_features=0 diagnostics=0 error_diagnostics=0
```

CTest now pins this harness with `scene3d-inspect-boxtextured-stats`, requiring
the same `meshes=1`, `vertices=24`, `indices=36`, and `textures=1` inspection
surface that future CLI/bake tooling can consume.

The same executable also accepts `--render-packet` to print the CPU-side
renderer input contract before any GPU upload:

```bash
./build-scene3d/core/scene/pulp-scene3d-inspect --render-packet \
  test/fixtures/scene3d/BoxTextured/BoxTextured.glb
```

That dump includes transformed node count, renderable primitive count, packet
diagnostics, and one row per renderable primitive with node/mesh indices,
material index, numeric feature mask, column-major world transform, and stable
`MaterialKey` feature names.
For the official fixture, CTest pins two transformed nodes, one renderable
indexed primitive, and the feature set
`normals,texcoord0,indexed,base_color_texture`.
`tools/scene3d/render_packet_graph_error_smoke.py`, wired through
`scene3d-render-packet-graph-error-contract`, pins the same CLI path for graph
error handling. It generates a loader-clean cyclic-node glTF and verifies that
`pulp-scene3d-inspect --render-packet` reports `scene.graph_cycle`,
`has_errors=true`, and zero render primitives without turning the loader-level
inspect stats into an error. `scene3d-render-packet-graph-error-negative-contract`
feeds fake inspect output through that same smoke so it must reject inspect exit
drift, loader-stats diagnostic drift, missing render packets, packet primitive
or diagnostic-count drift, `has_errors` drift, missing or renamed graph-cycle
diagnostics, and primitive rows leaking after graph errors.
`scene3d-render-packet-surface-contract` statically pins the renderer handoff
shape behind those behavior tests: `RenderPrimitive` and `RenderPacket` field
order, `build_render_packet()`'s validate-then-transform-then-filter sequence,
the `scene.render_packet_empty` diagnostic, and the
`pulp-scene3d-inspect --render-packet` output keys. This keeps the native
renderer-facing packet stable before adding more GPU proof cases.
`tools/scene3d/render_packet_matrix_smoke.py`, wired through
`scene3d-render-packet-matrix-contract`, pins matrix-form glTF node transforms
composed with child TRS into the render-packet primitive `world_transform`
field before any native renderer upload. `scene3d-render-packet-matrix-negative-contract`
feeds fake inspect output through that same smoke so it must reject inspect
exit drift, stats drift, missing or duplicated packet rows, packet error drift,
world-transform drift, feature-mask/name drift, and material fallback drift.

## glTF Validator Gate Slice

`tools/scene3d/validate_gltf.py` wraps the official Khronos glTF-Validator for
fixture and future bake-output validation. It supports either:

- a validator binary that handles `gltf_validator -o <asset>`, or
- the official npm package `gltf-validator` when it is resolvable to Node.

Both paths read the JSON report and fail if the validator reports any errors.

CTest now registers `scene3d-validate-boxtextured-gltf` against the official
`BoxTextured.glb` fixture. Local developer machines without the validator get a
CTest skip with a readable install/configuration hint. CI can make the gate hard
by invoking the same script with `--require-validator` or by ensuring
`gltf_validator` is on `PATH`.

CTest also registers `scene3d-gltf-validator-wrapper-contract`, which uses a
fake validator to pin the wrapper behavior without requiring network or npm
setup: missing validator returns the CTest skip code by default, missing
validator with `--require-validator` hard-fails, valid JSON with zero errors
passes, validator-reported errors fail, and malformed validator output fails.
`scene3d-gltf-validator-wrapper-negative-contract` feeds temporary wrapper
mutations through that verifier so it must reject missing-validator skip-code
drift, `--require-validator` exit drift, CLI invocation drift, success summary
label drift, validator-error exit drift, and malformed-report exit drift.

Manual hard gate:

```bash
python3 tools/scene3d/validate_gltf.py --require-validator \
  test/fixtures/scene3d/BoxTextured/BoxTextured.glb
```

Temporary npm-backed validation, without adding a repo dependency:

```bash
tmpdir=$(mktemp -d)
npm install --prefix "$tmpdir" gltf-validator@2.0.0-dev.3.10 --no-save
NODE_PATH="$tmpdir/node_modules" \
  python3 tools/scene3d/validate_gltf.py --require-validator \
    test/fixtures/scene3d/BoxTextured/BoxTextured.glb
```

`tools/scene3d/verify_bake_artifact.py` composes sidecar provenance,
validator, and native preflight contracts for future runtime-produced bake
outputs:

```bash
python3 tools/scene3d/verify_bake_artifact.py \
  --asset test/fixtures/scene3d/BoxTextured/BoxTextured.glb \
  --sidecar test/fixtures/scene3d/sidecars/clean.pulp3d.json \
  --preflight-tool build-scene3d/core/scene/pulp-scene3d-bake-preflight \
  --require-extensions \
  --require-runtime-evidence
```

It prints stable `key=value` fields for asset/sidecar existence, JSON validity,
supported artifact extensions (`.glb` / `.gltf` plus `.pulp3d.json`), exact
sidecar root/provenance/diagnostic/unsupported-feature/runtime-hint key
contracts, sidecar string value types, supported diagnostic severities,
supported schema version, required provenance fields (`source`, `exporter`,
`exported_at`), runtime-evidence presence, glTF-validator status, the exact
native preflight summary field set, preflight classification row counts matching
the summary counts, summary counts matching the sidecar diagnostic and
unsupported-feature arrays, classification coverage for every
`unsupported_features` entry, preflight readiness consistency with
`export_blocked` and `native_runtime_has_gaps`, preflight exit-code consistency
with `export_blocked`, preflight readiness, and
`bake_artifact_verified`. Missing Khronos validator remains a skip unless
`--require-gltf-validator` is passed. `--require-extensions` turns the
extension fields into an enforced artifact-shape gate. This keeps the native
lane ready to check GLB+sidecar pairs produced by the runtime exporter without
implementing `GLTFExporter`, `toDataURL`, `toBlob`, or live loader/capture
work here.
`verify_bake_artifact_contract.py`, wired through
`scene3d-verify-bake-artifact-contract`, mutates temporary sidecars to prove the
artifact checker itself rejects a wrong sidecar extension, an extra root key,
non-string runtime evidence, unsupported diagnostic severity, and non-string
runtime-hint values. It also rejects sidecars whose `unsupported_features`
entries do not appear in a preflight classification row, so future bake handoff
automation cannot treat unknown feature names as clean just because native
preflight ignored them. A synthetic fake-preflight case proves summary-count
drift is rejected when emitted classification rows disagree with the stable
`export_blockers`, `texture_encoding_blockers`, and `native_runtime_gaps`
summary fields. A second fake-preflight case proves summary counts must also
match the sidecar's raw `diagnostics` and `unsupported_features` arrays, so a
future preflight cannot hide sidecar content by reporting smaller counts.
Another fake-preflight case proves `bake_readiness` must be derived from
`export_blocked` and `native_runtime_has_gaps`, so summary flags and the
top-level verdict cannot drift independently.
A final fake-preflight case proves `export_blocked=true` must exit non-zero, so
future automation cannot silently accept blocked bake handoffs because the
process status was inconsistent with the summary fields.

## Renderer Fingerprint Golden Slice

`pulp-test-renderer3d` now pins interim RGBA fingerprints for the two
load-bearing native renderer proofs:

- `Renderer3D::render_hardcoded_textured_cube()` on a 128x128 offscreen target.
- Official `BoxTextured.glb` -> fastgltf -> `SceneData` ->
  `Renderer3D::render_scene_data()` on a 128x128 offscreen target.

These are macOS/default-Metal-adapter drift detectors, not the final
cross-platform golden lane. They intentionally use the existing
`HeadlessSurface::rgba_fingerprint()` FNV-1a helper instead of committing PNG
bytes while Phase 7 software-adapter selection is still missing. The tests
continue to soft-skip when Dawn/WebGPU is unavailable.

`Renderer3D` now carries Dawn adapter provenance in every successful render
result: backend, backend type, adapter name, vendor, and architecture. The
fingerprint assertions are gated to the Metal adapter that produced the interim
constants, so non-Metal or future software-adapter runs still exercise the
renderer structure without pretending the macOS Metal fingerprints are
portable. This keeps the Phase 7 gap explicit: adapter provenance exists, but
deterministic SwiftShader/software-adapter selection is still not implemented.

The first adapter-selection hook is also in place: `GpuSurface::Config` accepts
`force_fallback_adapter`, and `Renderer3D` render configs forward that opt-in to
Dawn. The renderer test suite includes a soft-skip fallback-adapter probe for
the hardcoded cube. A pass on the fallback path proves the host can return and
read back from a Dawn fallback adapter; a skip means the host has no fallback
adapter available. This still is not the final SwiftShader golden lane because
the code does not yet pin a specific software backend or check in a
software-adapter golden corpus.

`GpuSurface::Config` also accepts a backend preference with a `null_backend`
option, and `Renderer3D` now forwards an explicit null-backend preference from
its render configs. The null backend is useful for API/state validation and
adapter selection smoke tests, but it is not a pixel-producing golden path. The
renderer test suite probes the request both at the `GpuSurface` layer and
through `Renderer3D`, reporting a clean skip when the pinned Dawn/WebGPU package
does not provide a null adapter on the host.

Current fingerprints:

```text
hardcoded textured cube: 15925498132503966243
official BoxTextured fixture: 5845745157752120258
```

The current interim corpus is also recorded in
`test/fixtures/scene3d/renderer3d-goldens.json`. The manifest is intentionally
small: each entry records the renderer path, source asset, render size, adapter
backend type, adapter scope, `scene_data_consumed`, expected primitive count,
pipeline-cache entry/hit counts, minimum distinct/nontransparent pixel counts,
FNV-1a RGBA fingerprint, and the C++ constant that must match it.
`tools/scene3d/validate_renderer_golden_manifest.py`
validates that schema, rejects unexpected root, entry, and `software_adapter`
keys, restricts manifest status to `interim_default_adapter` or
`final_software_adapter`, and requires the current interim entries to remain
scoped to `macos_default_metal` / `Metal` with no pixel-producing software
adapter claim. It also pins renderer/source semantics: hardcoded-cube entries
must stay `source=hardcoded`, cannot claim `scene_data_consumed`, and cannot
record SceneData primitive or pipeline-cache counts; `render_scene_data` entries
must name a real scene source, claim `scene_data_consumed`, and record positive
primitive and pipeline-cache entry counts. For `render_scene_data`, the
pipeline-cache entry count cannot exceed the primitive count, and
`pipeline_cache_entry_count + pipeline_cache_hit_count` must equal the primitive
count, so every rendered primitive is accounted for as either a cache miss or a
cache hit. `scene3d-renderer-golden-manifest-malformed`
mutates temporary copies of the manifest to prove missing/extra root, entry,
and software-adapter keys are rejected; invalid status, duplicate IDs, interim
adapter drift, interim software-adapter claims, missing/mismatched C++ constants,
contradictory hardcoded-vs-SceneData fields, and cache-accounting drift are also
rejected through the executable validator. This keeps the manifest in
sync with `pulp-test-renderer3d`, giving the final software-adapter corpus a
concrete landing spot instead of another test-only constant.

The same validator now exposes an explicit final-golden gate:
`--require-pixel-software-adapter`. The current manifest is expected to fail
that gate because `software_adapter.pixel_producing` is false and no
software-scoped golden entries exist yet. CTest records that expected failure
through `scene3d-renderer-golden-software-adapter-required`, so interim macOS
Metal fingerprints can stay useful without accidentally satisfying Phase 7.
The manifest `status=final_software_adapter` is also self-enforcing: even
without the CLI gate, a final-status manifest must provide a pixel-producing
software adapter and matching software-scoped entries.
Dawn's null backend remains useful for API/state validation, but it is not a
pixel-producing software backend and cannot satisfy the final golden gate.
`verify_renderer_golden_manifest_final_adapter.py`, wired through
`scene3d-renderer-golden-final-adapter-contract`, exercises the future positive
path with a synthetic final software manifest and C++ fingerprint constant, then
proves the validator rejects a null backend, a missing software golden entry, a
software-adapter scope mismatch, and a final-status manifest that forgets the
pixel-producing software adapter contract. This keeps the final manifest rules
executable before a real pixel-producing backend is pinned.

`tools/scene3d/verify_renderer_probe.py` closes the loop between the manifest
and real probe output. It validates the manifest shape it relies on, runs
`pulp-renderer3d-probe`, parses the stable `key=value` fields, first verifies
the exact probe field set, and then checks the rendered scene, dimensions,
adapter backend, adapter scope, the Spike B
resource contract (color target, depth target, vertex/index/uniform buffers,
texture upload, command submission, readback, and pixel-output status),
structural fields (`scene_data_consumed`, primitive count), pipeline-cache
entry/hit counts, minimum pixel diversity/coverage, and RGBA fingerprint
against the selected manifest entry. The current CTest lane verifies both interim
macOS/default-Metal entries this way. Future
software-adapter entries can reuse the same tool once a pixel-producing
software backend is pinned.
`verify_renderer_probe_contract.py`, wired through
`scene3d-renderer-probe-verifier-contract`, feeds synthetic probe output through
the same verifier to prove malformed probe handoffs are rejected when a resource
field is missing, `scene_data_consumed` drifts, pixel output is false, or
coverage falls below the manifest floor. It also feeds malformed manifests
through the same verifier to prove missing/extra entry fields and invalid
software-adapter `golden_entry_ids` are rejected before probe output is trusted.
This keeps the manifest/probe handoff from becoming a positive-only smoke.
`verify_renderer_probe_manifest_schema.py`, wired through
`scene3d-renderer-probe-manifest-schema-contract`, keeps the manifest schema
constants duplicated inside `verify_renderer_probe.py` aligned with
`validate_renderer_golden_manifest.py`, so the probe verifier cannot quietly
accept a different manifest shape than the canonical manifest validator.
`scene3d-renderer-probe-manifest-schema-negative-contract` feeds temporary
probe/validator mutations through that alignment checker so it must reject
manifest-key drift, entry-key drift, missing probe constants, software-adapter
key drift, and missing validator constants.

`pulp-renderer3d-probe` exposes the same evidence at the command line for
handoffs and future manifest refreshes:

```bash
./build-scene3d-skia/core/render/pulp-renderer3d-probe \
  --scene boxtextured \
  --fixture test/fixtures/scene3d/BoxTextured/BoxTextured.glb \
  --width 128 --height 128 --adapter-scope macos_default_metal
```

The probe prints stable `key=value` fields for adapter provenance, resource
allocation/upload status, GPU submit/readback status, primitive/pipeline counts,
`rgba_fingerprint`, `pixel_output_produced`, and
`final_software_golden_eligible`. It also prints renderer-floor booleans for
texture decode/fallback, base-color sRGB upload, sampler handling and mipmap
downgrade, texture transform/TEXCOORD_1 routing, alpha/depth-write,
vertex-color, metallic-roughness, emissive, double-sided, normal, occlusion,
light/camera application/deferment, unsupported texture-route deferrals, advanced
material extension deferrals, and deferred animation, skinning, morph, and
instancing flags. `renderer_probe_material_floor_smoke.py`, wired
through `scene3d-renderer-probe-material-floor-contract`, pins those public
fields for the official `BoxTextured.glb` renderer path so probe output cannot
claim only pixel success while silently dropping the native material floor. On
the current macOS Metal interim lane the
final eligibility field is expected to remain false; passing
`--require-final-software-adapter` turns that into a non-zero exit so scripts
cannot accidentally promote hardware/default-adapter fingerprints to the final
software corpus.
`verify_renderer_probe_cli_surface.py`, wired through
`scene3d-renderer-probe-cli-surface-contract`, pins the probe's handoff CLI:
help text lists the public switches, invalid scenes and dimensions exit 64,
`boxtextured` requires `--fixture`, null-backend output reports
`pixel_output_produced=false` and exits non-zero, `--require-final-software-adapter`
exits 3 on the interim macOS adapter, `--output-png` writes the already encoded
native render PNG for human inspection, and the official `BoxTextured.glb` path
still exits 0 with scene-data consumption and pixel output. The probe's PNG
output path does not add fields to the golden-manifest probe surface; it is a
CLI evidence artifact for the native renderer proof.
`scene3d-renderer-probe-cli-surface-negative-contract` feeds fake probe
executables through that same verifier so it must reject help-surface drift,
usage/exit-code drift, missing fixture diagnostics, false null-backend pixel
claims, final-software gate drift, missing `scene_data_consumed` on the
`BoxTextured.glb` handoff path, and PNG output drift where the requested file is
missing or does not carry PNG magic.
`renderer_probe_material_floor_contract.py`, wired through
`scene3d-renderer-probe-material-floor-negative-contract`, feeds synthetic
probe output through that same material-floor verifier so it must reject missing
base-color sRGB fields, texture-upload drift, scene identity drift,
metallic-roughness factor drift, missing geometry normals, and false pixel
output before the official fixture path is trusted.
`verify_renderer_probe.py` now derives that expected eligibility from the
manifest's `final_software_adapter` status and `software_adapter.golden_entry_ids`
instead of hardcoding false. `verify_renderer_probe_final_eligibility.py`, wired
through `scene3d-renderer-probe-final-eligibility-contract`, uses a synthetic
final software manifest plus full-field fake probe output to prove future final
entries can verify only when the probe reports
`final_software_golden_eligible=true`.
`verify_renderer_probe_final_eligibility_contract.py`, wired through
`scene3d-renderer-probe-final-eligibility-negative-contract`, drives the same
probe verifier with synthetic final-software manifests so final eligibility
must depend on all three handoff conditions: `final_software_adapter` manifest
status, a pixel-producing software adapter, and membership in
`software_adapter.golden_entry_ids`.
`verify_renderer_probe_fake_surfaces.py`, wired through
`scene3d-renderer-probe-fake-surface-contract`, keeps the synthetic probe output
used by the probe-contract and final-eligibility tests aligned with
`PROBE_FIELDS`. `scene3d-renderer-probe-fake-surface-negative-contract` feeds
temporary mutations through that checker so it must reject dropped probe-contract
fields, dropped final-eligibility resource/final fields, and newly added probe
fields that the synthetic fakes do not yet emit.
`verify_renderer_probe_public_surface.py`, wired through
`scene3d-renderer-probe-public-surface-contract`, parses `renderer3d.hpp`,
`renderer3d_probe.cpp`, and `verify_renderer_probe.py` to prove every public
`HardcodedCubeRenderResult` boolean is both printed by the probe and accepted by
the strict verifier. This keeps newly added renderer facts from remaining
private to C++ while probe handoffs keep passing.
`verify_renderer_probe_public_surface_contract.py`, wired through
`scene3d-renderer-probe-public-surface-negative-contract`, mutates temporary
copies of those files to prove the guard rejects removed header booleans,
removed probe output, removed verifier fields, and mismatched probe key/member
pairs.
`verify_renderer_probe_fake_surfaces.py`, wired through
`scene3d-renderer-probe-fake-surface-contract`, keeps the synthetic probe output
used by `verify_renderer_probe_contract.py` and
`verify_renderer_probe_final_eligibility.py` aligned with the strict
`PROBE_FIELDS` set, so future fake-probe contracts cannot silently drift back to
an older shorter handoff surface.
`verify_renderer_probe_route_inventory.py`, wired through
`scene3d-renderer-probe-route-inventory-contract`, keeps renderer probe route
telemetry from becoming an ad hoc checklist. It parses `PROBE_FIELDS`, excludes
generic adapter/output fields, and requires every remaining telemetry field to
be mentioned by at least one `renderer_probe_*_route_smoke.py` or
`renderer_probe_*_route_contract.py` script. It also requires every route to
have both a positive smoke and a negative contract, and requires every route
smoke and negative-contract script to be registered in `test/CMakeLists.txt`.
`scene3d-renderer-probe-route-inventory-negative-contract` feeds temporary
probe, CTest, and route-file mutations through the checker so missing
route-field coverage, missing route-test registration, missing negative
contracts, and missing positive smokes are all rejected. The current inventory
is 74 route-specific telemetry fields across 42 route files / 21 route pairs.
`verify_native_slice_handoff.py`, wired through
`scene3d-native-slice-handoff-contract`, keeps the first native slice handoff
auditable as a durable artifact set: SceneData, native source and link-boundary
checkers, the hardcoded `scene3d-renderer-probe-hardcoded` and BoxTextured
`scene3d-renderer-probe-boxtextured` renderer probes,
`scene3d-renderer-probe-public-surface-contract`, fake-probe coverage,
`scene3d-renderer-probe-manifest-schema-contract`,
`scene3d-renderer-probe-final-software-gate`, route inventory, bake-preflight
URL gating, bake-artifact verification, sidecar schema constants, and the #2738
/ runtime #3369 evidence links must all stay present together. The runtime
dependency is pinned to the exact runtime evidence comment
`https://github.com/Generous-Corp/pulp/issues/3369#issuecomment-4610177927`,
not only the issue-level URL, so the native lane consumes the sibling evidence
without reimplementing Live `GLTFLoader`. The same exact URL must remain in
the CTest sidecar/preflight handoff and in the clean sidecar fixture used by
the bake-artifact verifier, so runtime evidence drift is caught in executable
inputs as well as documentation. The handoff also pins
`final_software_golden_eligible=false` while the branch uses interim
default-adapter fingerprints instead of claiming final software-adapter goldens.
`scene3d-native-slice-handoff-negative-contract` mutates temporary docs,
CTest wiring, and renderer artifacts so missing issue evidence, missing URL-gate
documentation, missing exact runtime evidence comment links,
missing exact CTest/fixture runtime evidence links, missing final-software-gate
documentation, missing probe registrations, and missing renderer probe source
are rejected before a handoff claims first-slice coverage.

`scene3d-renderer-probe-null-backend-api` covers the corresponding CLI path:
the probe accepts `--adapter-backend null`, prints
`adapter_backend_type=Null`, `null_backend_requested=true`, and
`pixel_output_produced=false`, and exits as a non-pixel render. This keeps Dawn
null backend coverage available for API/state validation while preserving the
final Phase 7 requirement for a pixel-producing software adapter.

## Animation Data Slice

`SceneData` now stores parsed glTF animation data for the first bake/runtime
planning boundary:

- Samplers carry input keyframe times, flattened output values, output
  component counts, and interpolation mode.
- Channels reference valid nodes and locally compacted sampler indices.
- Translation, rotation, and scale paths are accepted; morph weight channels
  warn with `gltf.animation_path_unsupported`.
- Validation checks that sampler keyframe counts and component groupings are
  structurally coherent.

This is intentionally data-only. It does not advance animation playback,
skinning, morph targets, shader updates, or render-time sampling. The renderer
now reports preserved transform animations as deferred so this data is not
silently ignored on the native path.
`tools/scene3d/animation_contract_smoke.py`, wired through
`scene3d-animation-contract`, creates a tiny animated glTF and verifies the
CLI handoff: `pulp-scene3d-inspect` reports one preserved animation and one
unsupported weights path, `pulp-scene3d-sidecar` emits both the deferred
`TransformAnimation` and `AnimationPath:weights` native gaps plus
`animationCount=1`, and `pulp-scene3d-bake-preflight` classifies the sidecar as
`native_gaps` instead of export-blocked.

## Material Key Slice

`core/scene/include/pulp/scene/material_key.hpp` now defines a CPU-only
`MaterialKey` for the first native renderer pipeline-cache boundary:

- Primitive-layout flags: normals, tangents, TEXCOORD_0, TEXCOORD_1, COLOR_0,
  and indexed geometry.
- Material flags: base-color texture, metallic-roughness texture, normal
  texture, occlusion texture, emissive texture, unlit, double-sided, alpha
  blend, alpha mask, base-color texture transform, non-base-color texture
  transform, non-base-color TEXCOORD_1 routing, normal scale, and occlusion
  strength.
- Fallback flag when a primitive has no valid material.

`derive_material_key()` consumes only `SceneData` and `PrimitiveData`, so it
does not retain glTF structs or create GPU resources. Tests pin the textured
unlit/double-sided/alpha/PBR-texture path and ensure invalid texture references
do not accidentally select a textured pipeline variant.
`tools/scene3d/render_packet_material_key_smoke.py`, wired through
`scene3d-render-packet-material-key-contract`, now carries that proof through
the public `pulp-scene3d-inspect --render-packet` CLI path. The smoke builds a
temporary rich PBR glTF with tangents, TEXCOORD_1, COLOR_0, five texture slots,
texture transforms, unlit/double-sided/alpha state, normal scale, and occlusion
strength, then asserts the exact non-fallback feature mask and feature-name
list that the native renderer pipeline cache will consume.
`scene3d-render-packet-material-key-negative-contract` feeds fake inspect output
through that same smoke so it must reject inspect exit drift, texture-count and
texture-byte stats drift, extra stats keys, missing or duplicated packet rows,
packet error drift, feature-mask drift, missing or reordered feature names,
material-index drift, and world-transform drift.
`scene3d-material-feature-surface-contract` statically pins the same public
surface at the source level: every `MaterialFeature` bit assignment in
`material_key.hpp` must stay aligned with the exported feature-name order in
`material_key.cpp`. This keeps the renderer pipeline-cache key stable even
before a new glTF fixture exercises a changed feature flag.
`scene3d-material-feature-surface-negative-contract` feeds temporary
`material_key` header/source mutations through that verifier so it must reject
bit-assignment drift, missing or reordered feature flags, feature-name text
drift, missing feature-name rows, and duplicate feature-name rows.
`scene3d-render-packet-surface-negative-contract` does the same for the native
renderer handoff packet: temporary `render_packet` and inspect-source mutations
must be rejected when primitive fields, packet field order, diagnostic gating,
empty-packet diagnostics, CLI feature-mask keys, or feature-name handoff drift.

## Sidecar JSON Slice

`core/scene` now has a deterministic `*.pulp3d.json` sidecar wire format:

- `schema_version: 1`.
- `provenance` with source, exporter, exported timestamp, and an optional
  `runtime_evidence` URL pointing at the runtime-hardening issue/comment that
  proved the live GLB/export prerequisite for this sidecar.
- `diagnostics` with severity, code, message, and `path`. The parser also
  accepts the legacy `source_path` key for older generated sidecars, but the
  serializer now emits `path` to match the checked-in bake-preflight matrix
  fixtures.
- `unsupported_features` with feature, reason, and node path.
- `runtime_hints` with key/value pairs.
- `build_sidecar_from_scene()` copies loader diagnostics, appends structural
  validation diagnostics, records native-renderer-floor unsupported features,
  and emits camera/light/animation hints for bake/runtime handoff.
- Sidecar analysis now records only the remaining deferred non-base-color
  material texture routes separately from the texture slots: unsupported
  coordinate sets beyond the renderer floor, missing authored-or-derivable
  normal texture tangent basis, normal scale when the normal texture cannot be
  applied, and occlusion strength when the occlusion texture cannot be applied.
  TEXCOORD_1 and `KHR_texture_transform` routes are no longer reported as
  native gaps when the primitive provides the requested coordinate set.

The implementation uses the repo's existing `choc::json` path rather than
manual string concatenation, so string escaping and parsing match other Pulp
serialization surfaces. Tests cover round-tripping escaped strings, preserving
diagnostic severities, preserving runtime evidence provenance links,
preserving unsupported-feature reports/runtime hints, preserving analyzed
camera/light/animation runtime hints through JSON serialization and parsing,
analyzing parsed `SceneData` into sidecar diagnostics, and rejecting missing
schema versions, missing root keys, unexpected object keys, non-string sidecar
values, and unsupported diagnostic severities. This gives the bake/export lane
a native-owned sidecar target even while live `GLTFExporter` texture export and
the shared `BoxTextured.glb` fixture remain runtime/shared coordination work.

`pulp-scene3d-sidecar` exposes the same sidecar builder at the command line for
already-authored GLB/GLTF input:

```bash
./build-scene3d/core/scene/pulp-scene3d-sidecar \
  --source khronos-boxtextured \
  --runtime-evidence https://github.com/Generous-Corp/pulp/issues/2738 \
  test/fixtures/scene3d/BoxTextured/BoxTextured.glb
```

The tool emits valid `*.pulp3d.json` to stdout, preserves explicit provenance
fields, and records the native analyzer's diagnostics, unsupported features,
and runtime hints. It does not invoke or implement `GLTFExporter`,
`toDataURL`, `toBlob`, `TextureLoader`, `GLTFLoader`, or any web-compat bridge
code.

`scene3d-sidecar-cli-contract` pins the producer-side provenance rules:
`--exported-at` must be provided and non-empty, `--exporter` cannot be forced to
an empty value, and an empty `--source` falls back to the input asset path.
Runtime evidence remains optional at this producer layer because
`--require-runtime-evidence` is the stricter preflight/artifact verification
mode.

`scene3d-sidecar-preflight-boxtextured-clean` now proves that the sidecar
emitted for the official `BoxTextured.glb` fixture round-trips through
`pulp-scene3d-bake-preflight --require-runtime-evidence` as clean: no export
blockers, no texture-encoding blockers, no native runtime gaps, no missing
runtime evidence, and no error diagnostics. The direct sidecar CTest and the
round-trip smoke pin the provenance fields that future bake outputs need for
handoff: `source`, `exporter`, `exported_at`, and `runtime_evidence`.

Bake/export boundary after runtime evidence:

- Runtime #3369 now carries live GLTF loader/fetch/input, `gltf-box` textured
  capture, and DataTexture upload evidence, including
  `https://github.com/Generous-Corp/pulp/issues/3369#issuecomment-4610177927`,
  so the native sidecar can record that issue/comment as provenance for future
  baked GLBs.
- This branch still does not implement `GLTFExporter`, `toDataURL`, `toBlob`,
  `TextureLoader`, `GLTFLoader`, or web-compat bridge code; those remain in the
  runtime-hardening lane.
- A baked asset produced by the live lane should write the runtime evidence URL
  into `provenance.runtime_evidence`, then use the existing diagnostics and
  unsupported-feature arrays for non-bakeable `ShaderMaterial`,
  postprocessing, render targets, missing texture-encode support, and native
  renderer-floor gaps.
- `verify_bake_artifact.py --require-runtime-evidence-url` now makes that
  handoff executable for final bake-artifact checks: the clean artifact fixture
  must carry an `http`/`https` runtime evidence URL, and the malformed-artifact
  contract rejects placeholder tokens such as `native-preflight-fixture`.
- `append_bake_unsupported_feature()` now standardizes those bake/export
  diagnostics for the future exporter/CLI path without depending on Three.js or
  web-compat types. It records warning diagnostics and sidecar unsupported
  features for `ShaderMaterial`, `RawShaderMaterial`, postprocessing /
  `EffectComposer`, render targets, arbitrary JS animation, physics, event
  handlers, and missing texture encoding (`toDataURL`/`toBlob` or pre-encoded
  buffers).
- `scene3d-bake-unsupported-surface-contract` statically pins that future
  exporter handoff surface: the `BakeUnsupportedFeature` enum, descriptor
  feature names, diagnostic codes, key user-facing reason text, and the
  warning-diagnostic plus unsupported-feature append path must stay aligned.
- `scene3d-bake-unsupported-surface-negative-contract` feeds temporary sidecar
  header/source mutations through that verifier so it must reject enum removal
  and order drift, descriptor feature/code/reason drift, missing descriptor
  cases, unsupported-feature reason drift, diagnostic severity drift, and
  diagnostic-code drift.
- `scene3d-sidecar-verifier-contract` now feeds fake sidecar executables through
  the BoxTextured sidecar verifier so it must reject missing runtime-hint arrays,
  extra root keys, exporter drift, empty source/exported-at/runtime-evidence
  provenance, non-empty diagnostics for the clean fixture, and malformed JSON.
- `analyze_bake_preflight()` provides the native-owned Phase 5 preflight
  contract over `SidecarData`. It classifies sidecar features into export
  blockers, texture-encoding blockers, and native renderer gaps so future
  exporter/CLI code can decide whether a scene is bake-ready without importing
  Three.js runtime, `GLTFExporter`, `toDataURL`, `toBlob`, `TextureLoader`,
  `GLTFLoader`, or web-compat bridge code.
- `scene3d-bake-preflight-classification-surface-contract` statically pins the
  C++ predicate buckets in `bake_preflight.cpp`: export blockers, texture
  encoding blockers, native gap exact features, and native gap prefixes. The
  fixture matrix proves concrete behavior, while this verifier prevents bucket
  drift in the public bake handoff categories before a new fixture is added.
- `scene3d-bake-preflight-classification-surface-negative-contract` feeds
  temporary classification mutations through that verifier so it must reject
  missing or extra export blockers, texture-encoding bucket drift, missing or
  extra native gaps, missing native-gap prefixes, extra export-blocker prefixes,
  and renamed native-gap prefixes.
- `Renderer3D deferred flags stay aligned with sidecar native gaps` now ties
  the renderer proof back to that bake/preflight contract. It renders existing
  synthetic `SceneData` fixtures and then builds a sidecar from the same data,
  asserting that deferred renderer facts for skinning, morph targets, GPU
  instancing, transform animation, unsupported non-base-color texture routes,
  missing normal texture tangent basis, normal scale, and occlusion strength
  become `native_gaps` rather than silent clean bake readiness.
- `SidecarData analyzer omits default-path PBR texture gaps` pins the inverse
  contract: tangent-bearing primitives with default TEXCOORD_0
  metallic-roughness, normal, occlusion, and emissive texture slots do not emit
  sidecar unsupported-feature rows, and `analyze_bake_preflight()` reports
  `clean`.
- `pulp-scene3d-bake-preflight <scene.pulp3d.json>` exposes that preflight
  contract at the command line. It prints stable summary fields for export
  blockers, texture-encoding blockers, native runtime gaps, and error
  diagnostics. The command exits non-zero only when export is blocked; native
  runtime gaps remain warnings because they do not prevent producing a portable
  GLB.
- The command also prints `bake_readiness=blocked|native_gaps|clean` as the
  top-level verdict for scripts. `blocked` means GLB export/bake readiness is
  blocked, `native_gaps` means a portable GLB can be produced but native
  no-JS playback still has known gaps, and `clean` means neither condition was
  reported.
- `scene3d-bake-preflight-cli-surface-contract` statically pins that CLI
  handoff: usage/help for `--require-runtime-evidence`, parse/read failure
  diagnostics, stable summary field order, export/texture/native row labels,
  and the exit-code distinction between export-blocked assets and native-only
  runtime gaps.
- `scene3d-bake-preflight-cli-surface-negative-contract` feeds temporary
  `scene3d_bake_preflight` source mutations through that verifier so it must
  reject usage-option drift, read-file diagnostic drift, runtime-evidence flag
  drift, sidecar parser drift, readiness-key drift, missing or extra summary
  fields, feature-row reason drift, and blocked exit-code drift.
- `scene3d-bake-preflight-fixture-cli-contract` runs the public
  `pulp-scene3d-bake-preflight` executable over the checked-in sidecar fixture
  set and pins each fixture's exit code plus key summary rows: clean assets
  remain clean, ShaderMaterial/live-runtime/texture-encoding blockers exit
  blocked, native-only gaps stay warning-only, and `--require-runtime-evidence`
  turns a missing provenance link into an export-blocked result.
- `scene3d-bake-preflight-fixture-cli-negative-contract` feeds fake preflight
  executables through that verifier so it must reject clean/native-gap exit-code
  drift, missing ShaderMaterial and texture-encoding rows, live-runtime blocker
  count drift, and a missing `--require-runtime-evidence` result.
- `scene3d-bake-preflight-malformed-sidecars` now writes temporary malformed
  sidecars and proves the `pulp-scene3d-bake-preflight` executable rejects
  non-integer and unsupported schema versions, numeric provenance values, empty
  required provenance values (`source`, `exporter`, `exported_at`), unsupported
  diagnostic severities, extra diagnostic keys, missing root keys, non-string
  unsupported-feature values, and non-string runtime-hint values. Empty
  `runtime_evidence` is still allowed at parse time because
  `--require-runtime-evidence` is the stricter preflight/artifact gate. This
  pins the executable preflight path to the same strict sidecar contract as the
  C++ parser and artifact verifier without adding or mutating shared fixtures.
- `scene3d-verify-bake-artifact-contract` now also mutates the checked-in clean
  sidecar into empty `source`, `exporter`, and `exported_at` provenance variants
  and proves the artifact verifier rejects each one before future bake/export
  output can pass with anonymous provenance.
- `pulp-scene3d-bake-preflight --require-runtime-evidence` adds one stricter
  bake-readiness check: sidecars without `provenance.runtime_evidence` report
  `runtime_evidence_missing=true` and are treated as export-blocked. This is
  covered by a matrix verifier that also rejects unexpected root, provenance,
  diagnostic, unsupported-feature, and runtime-hint JSON keys so sidecar schema
  drift cannot hide behind unchanged preflight summary output.
- `pulp-scene3d-bake-preflight --require-runtime-evidence-url` adds the
  final-handoff variant of that gate: the evidence must be a non-empty
  `http://` or `https://` URL with a host and path. Non-URL sentinel values
  such as `native-preflight-fixture` now report
  `runtime_evidence_url_invalid=true` and are treated as export-blocked. This
  mirrors `verify_bake_artifact.py --require-runtime-evidence-url` in native
  C++ so runtime-produced bake artifacts cannot satisfy the final handoff with
  a local placeholder token. The bake-artifact verifier now forwards that URL
  gate to `pulp-scene3d-bake-preflight` and treats
  `runtime_evidence_url_invalid` as part of the exact preflight handoff schema,
  so verifier-level URL checks and native preflight output cannot drift apart.
  Both runtime-evidence gates remain opt-in so direct GLB/native analysis can
  still run before live exporter evidence exists.
- Export blockers are live-only material/runtime constructs such as
  `ShaderMaterial`, `RawShaderMaterial`, postprocessing, render targets,
  arbitrary JavaScript animation, physics, and event handlers. Missing texture
  encoding is tracked separately because it is an exporter capability gap,
  while parsed-but-deferred native renderer features such as KTX2 payloads,
  transform animation playback, advanced material extensions, morph targets,
  skinning, and GPU instancing do not by themselves claim that `GLTFExporter`
  could not emit an asset.
- `tools/scene3d/unsupported_mesh_contract_smoke.py`, wired through
  `scene3d-unsupported-mesh-contract`, now pins that public CLI handoff for
  morph weights, morph targets, skinning, and `EXT_mesh_gpu_instancing`:
  `pulp-scene3d-inspect` reports four warning diagnostics and no errors,
  `pulp-scene3d-sidecar` carries the four native gap rows, and
  `pulp-scene3d-bake-preflight` classifies them as `native_gaps` rather than
  export blockers.
- `scene3d-bake-preflight-live-runtime-blocked` now exercises the Live-only
  blocker class through a sidecar fixture and the native CLI: RawShaderMaterial,
  postprocessing, render targets, arbitrary JS animation, physics, and event
  handlers all report as export blockers, while texture encoding remains in its
  separate blocker bucket.

Current sidecar analyzer floor:

- Texture payloads with no encoded bytes are reported as unsupported.
- Texture formats outside PNG/JPEG, such as KTX2, are reported as unsupported
  for the current native renderer floor.
  `tools/scene3d/texture_payload_format_contract_smoke.py`, wired through
  `scene3d-texture-payload-format-contract`, pins that public handoff with a
  loader-clean generated glTF: an embedded `image/ktx2` payload and an empty
  embedded PNG payload become `TextureFormat:image/ktx2` and `TexturePayload`
  native runtime gaps, not export blockers.
- Default-path metallic-roughness, normal, occlusion, and emissive textures are
  no longer reported as unsupported because the native renderer now samples
  those slots when their required inputs are present.
- `default-pbr-textures-clean.pulp3d.json` keeps that contract in the CLI
  preflight matrix: the fixture carries a diagnostic marker for the material
  floor and a value-pinned `materialTextureFloor=default-pbr-textures` runtime
  hint but no unsupported-feature rows, and `pulp-scene3d-bake-preflight` still
  reports `bake_readiness=clean`.
- Normal textures used by primitives without authored or derivable tangent data
  are reported as unsupported because the current normal-map path requires a
  usable tangent basis.
  `tools/scene3d/material_texture_gap_contract_smoke.py`, wired through
  `scene3d-material-texture-gap-contract`, pins the user-visible
  classification for this material floor: embedded PNG slots avoid texture
  payload noise, while normal-map tangent-basis requirements and normal scale
  remain `native_gaps` instead of export blockers for the synthetic fixture
  with degenerate normal UVs. The same smoke explicitly rejects stale native-gap
  rows for supported non-base-color texture transforms, supported non-base
  `TEXCOORD_1` routing, and consumed occlusion strength so sidecar and
  preflight output stay aligned with the current renderer route floor.
- Advanced physical-material extensions are reported as deferred through loader
  diagnostics and sidecar unsupported-feature records.
  `tools/scene3d/advanced_material_contract_smoke.py`, wired through
  `scene3d-advanced-material-contract`, pins that public handoff for ten
  `KHR_materials_*` extensions: inspect reports warning diagnostics with no
  errors, sidecar emits one `MaterialExtension:*` native-gap row per extension,
  and preflight classifies the asset as `native_gaps` rather than
  export-blocked.
- Valid glTF primitive modes outside the current indexed-triangle renderer
  floor, such as `LINES`, are reported as `PrimitiveMode:*` native runtime
  gaps instead of export blockers. The loader preserves the scene and skips the
  non-renderable primitive for this slice.
  `tools/scene3d/primitive_mode_contract_smoke.py`, wired through
  `scene3d-primitive-mode-contract`, now pins that public handoff end to end:
  `pulp-scene3d-inspect` reports warnings and no errors for a valid `LINES`
  asset, `pulp-scene3d-sidecar` carries `PrimitiveMode:Lines`, and
  `pulp-scene3d-bake-preflight` classifies the asset as `native_gaps` rather
  than export-blocked.
- Draco-compressed primitives can flow into normal `SceneData` through a
  supplied loader callback; `pulp-render` now provides a callback adapter for
  its Draco decoder, while the default native loader path still reports missing
  callback wiring explicitly.
- Parsed transform animations are reported as unsupported for native playback;
  the data is preserved in `SceneData`, but render-time sampling is not wired.
- Valid glTF animation channel paths outside the current TRS preservation
  floor, such as `weights`, are reported as `AnimationPath:*` native runtime
  gaps even when no transform animation channel survives into `SceneData`.
- Additional node-attached directional/point/spot light occurrences beyond
  the current renderer's single consumed occurrence of that type, including
  duplicate node references to the same light record, are reported as
  `PunctualLight:*` native runtime gaps. The directional slot prefers the
  first node-attached directional occurrence; if none exists, the first
  directional light record is used as the fallback and is not reported as a
  gap. Additional fallback directional records are reported as additional
  directional gaps, while unattached directional records are reported as
  unattached gaps when a node-attached directional light already owns the
  renderer slot. Unattached point/spot lights are reported as gaps because
  the current native renderer consumes only node-attached point/spot
  occurrences.
- Scaled or non-rigid light node transforms are reported as
  `PunctualLight:nodeTransform` native runtime gaps. Rigid translation and
  rotation remain consumed by the current renderer floor for punctual light
  position/direction, but scale/shear/perspective matrix components are only
  preserved for handoff diagnostics.
  `tools/scene3d/light_node_transform_gap_contract_smoke.py`, wired through
  `scene3d-light-node-transform-gap-contract`, pins the public CLI handoff for
  scaled directional and sheared point-light nodes: inspect stays loader-clean,
  sidecar emits the two light-transform native gaps, and preflight classifies
  them as `native_gaps` rather than export blockers.
- Non-rigid/scaled camera node transforms and additional preserved camera
  occurrences beyond the first occurrence,
  including duplicate node references to the same camera record, are reported
  as `Camera:*` native runtime gaps because the current native renderer proof
  consumes only the first camera's projection scale, aspect, depth range, and
  view-basis floor, not full view/projection matrices or multi-camera selection.
  `tools/scene3d/camera_light_gap_contract_smoke.py`, wired through
  `scene3d-camera-light-gap-contract`, pins the multi-camera/light public
  handoff: additional and duplicate node-attached punctual lights, unattached
  punctual lights, non-rigid camera node transforms, additional camera
  occurrences, and duplicate camera references all stay `native_gaps` rather
  than export blockers.
- The first camera node, light count, and animation count are emitted as runtime
  hints.
  `tools/scene3d/camera_light_contract_smoke.py`, wired through
  `scene3d-camera-light-contract`, now pins the public CLI handoff for a valid
  perspective-camera plus directional-light asset: `pulp-scene3d-inspect`
  reports one camera and one light with no loader diagnostics,
  `pulp-scene3d-sidecar` emits `preferredCamera=CameraNode` and `lightCount=1`,
  and `pulp-scene3d-bake-preflight` classifies the sidecar as clean because the
  current native renderer floor consumes the first camera's depth metadata and
  the first directional light.

The CPU-side loader contract is now test-pinned by `pulp-test-scene3d`:

- root, child, mesh, material, texture, and primitive index references must stay
  in range.
- node camera and light references must stay in range.
- glTF matrix node transforms must be preserved and consumed by scene graph
  traversal.
- animation channels must reference valid nodes and local animation samplers.
- animation sampler input/output keyframe counts must stay coherent.
- positions must be non-empty XYZ data.
- normals and TEXCOORD_0 must match the position vertex count when present.
- tangents must match the position vertex count as XYZW data when present.
- TEXCOORD_1 and COLOR_0 must match the position vertex count when present.
- the first renderer slice requires indexed triangle primitives.
- material keys derive stable feature masks for the renderer pipeline cache.
- PBR material factors and texture slots are preserved at the native loader
  boundary without claiming full PBR shading.
- sidecar data carries provenance, unsupported features, runtime hints, and
  diagnostics without depending on web-compat or renderer types.
- sidecar analysis turns parsed scene state into bake/native diagnostics
  without depending on live Three.js exporter code.
- bake preflight classifies export blockers, texture encoding gaps, structural
  error diagnostics, and native renderer gaps without depending on live Three.js
  exporter code.
- `scene3d-native-source-boundary-negative-contract` mutates temporary native
  Scene3D/Renderer3D sources to prove the source-boundary checker rejects new
  source files, missing expected sources, view/widget bridge references, JS
  engine references, Live Three.js loader/exporter references, parser leaks into
  non-loader scene sources or renderer sources, and GPU leaks into CPU scene
  sources. It also proves fastgltf remains allowed only in the loader and WebGPU
  types remain allowed in the renderer.

Validation run:

```bash
cmake --build build-scene3d --target pulp-test-scene3d pulp-test-renderer3d -j8
./build-scene3d/test/pulp-test-scene3d --reporter compact
./build-scene3d/test/pulp-test-renderer3d --reporter compact

cmake --build build-scene3d-skia --target pulp-test-scene3d pulp-test-renderer3d -j8
./build-scene3d-skia/test/pulp-test-scene3d --reporter compact
./build-scene3d-skia/test/pulp-test-renderer3d --reporter compact
```

Current result:

- `pulp-test-scene3d` passed in both caches: 246 assertions in 17 test cases.
- no-Skia `pulp-test-renderer3d` passed the intended soft-skip path: 5
  assertions in 3 test cases.
- Skia-backed `pulp-test-renderer3d` passed on Metal: 54 assertions in 3 test
  cases.
- `python3 tools/deps/audit.py --strict --format markdown` passed with
  fastgltf and simdjson covered by the dependency docs and notice.
- Release flags were verified in the Skia-backed cache for `pulp-scene`,
  fastgltf, simdjson, and `pulp-render` as `-O3 -DNDEBUG`.
- `pulp-test-renderer3d` links `pulp::scene` only because its test fixture calls
  `load_gltf_scene()`; `pulp-render` itself does not link fastgltf or simdjson.

## Dependency On Live GLB Evidence

The native lane will not reimplement live `GLTFLoader` or runtime WebGPU
hardening. Per `threejs-parallel-coordination.md`, native
`BoxTextured.glb` rendering does not wait on live `GLTFLoader`; live success
gates Live-mode claims and later parity checks, not native fastgltf rendering.
The coordinated `BoxTextured.glb` fixture decision has been recorded on #2738
and the official Khronos fixture now feeds the native loader/render validation
path. The remaining dependency on the runtime-hardening lane is for live-mode
claims and later live-vs-native parity, not for native fastgltf rendering.

Until then, the native proof renders a generated embedded GLB through
fastgltf-backed `SceneData`, and the hardcoded cube remains as the isolated
renderer smoke.

The first-slice handoff verifier now audits
`planning/threejs-webgpu-gltf-bake-plan.md` as well as this analysis document,
CTest, and the clean sidecar fixture. That keeps the checked-out native plan
aligned with the runtime-hardening ownership boundary: live
`GLTFLoader + BoxTextured.glb` stays runtime-owned, native
`BoxTextured.glb` rendering does not wait on live `GLTFLoader`, and the exact
runtime evidence URL remains pinned for Live-mode and later parity claims.
`tools/scene3d/verify_native_slice_handoff_contract.py` includes a
`missing-plan-runtime-boundary` drift case so future edits cannot silently move
live-loader work back into this native lane.

## Native Transform Animation Route

The command-line renderer probe now has a generated glTF route for transform
animation. `renderer_probe_transform_animation_route_smoke.py` creates a
temporary triangle scene with one translation animation channel, then verifies
that the native renderer consumes the first keyframe as the initial node pose
while still reporting full transform-animation playback as deferred. The route
also asserts that camera, light, skinning, morph-target, GPU-instancing, alpha,
vertex-color, unlit, double-sided, emissive, normal-map, and non-base-color
texture-extension telemetry does not leak into the transform-animation proof.

`renderer_probe_transform_animation_route_contract.py` negative-tests that
route by mutating fake probe output for initial-pose drift, deferred-playback
drift, camera/light leaks, skinning deferral leaks, primitive-count drift, and
pixel-output drift. CTest wires these as
`scene3d-renderer-probe-transform-animation-route-contract` and
`scene3d-renderer-probe-transform-animation-route-negative-contract`.

## Native Advanced Material Extension Route

The command-line renderer probe now has a generated glTF route for advanced
PBR material extensions. `renderer_probe_advanced_material_route_smoke.py`
creates a temporary triangle scene whose material uses
`KHR_materials_clearcoat` and `KHR_materials_transmission`, then verifies that
the native renderer reports those extensions through
`advanced_material_extension_deferred` while still rendering the metallic-
roughness floor with the fallback texture and base-color factor. The same route
also pins that camera, light, transform-animation, alpha, vertex-color, unlit,
emissive, normal-map, non-base-color texture-extension, skinning, morph-target,
and GPU-instancing telemetry does not leak into this proof.

`renderer_probe_advanced_material_route_contract.py` negative-tests fake probe
output for advanced-material deferral drift, double-sided route drift,
unexpected texture decode, unlit/camera leaks, primitive-count drift, and
pixel-output drift. CTest wires these as
`scene3d-renderer-probe-advanced-material-route-contract` and
`scene3d-renderer-probe-advanced-material-route-negative-contract`.

## Native Unsupported Mesh Feature Route

The command-line renderer probe now has a generated glTF route for unsupported
mesh feature deferrals. `renderer_probe_unsupported_mesh_route_smoke.py`
creates a temporary triangle scene with primitive morph targets, a node skin,
and `EXT_mesh_gpu_instancing` attributes on the same renderable node. The route
verifies that the native renderer still renders the base triangle while
reporting `skinning_deferred`, `morph_target_deferred`, and
`gpu_instancing_deferred` as native runtime gaps. It also pins that camera,
light, transform-animation, alpha, vertex-color, unlit, double-sided,
emissive, normal-map, non-base-color texture-extension, and advanced-material
telemetry does not leak into this proof.

`renderer_probe_unsupported_mesh_route_contract.py` negative-tests fake probe
output for missing skinning, morph-target, and GPU-instancing deferrals,
animation and advanced-material leaks, primitive-count drift, and pixel-output
drift. CTest wires these as
`scene3d-renderer-probe-unsupported-mesh-route-contract` and
`scene3d-renderer-probe-unsupported-mesh-route-negative-contract`.

## Native Renderer Resource Floor Route

The command-line renderer probe now has a generated glTF route for the native
renderer resource floor. `renderer_probe_resource_floor_route_smoke.py` creates
a temporary triangle scene with a simple metallic-roughness material, then
verifies the render path allocates color and depth targets, uploads vertex,
index, and uniform buffers, uploads the fallback texture, submits commands, and
produces readback pixels. The route also pins the base-color and
metallic-roughness factor handoff while asserting that optional texture decode,
camera, light, animation, alpha, vertex-color, unlit, double-sided, emissive,
normal-map, non-base-color texture-extension, advanced-material, skinning,
morph-target, and GPU-instancing telemetry does not leak into this proof.

`renderer_probe_resource_floor_route_contract.py` negative-tests fake probe
output for color/depth target allocation drift, vertex/index/uniform upload
drift, metallic-roughness factor drift, unexpected texture decode,
primitive-count drift, and pixel-output drift. CTest wires these as
`scene3d-renderer-probe-resource-floor-route-contract` and
`scene3d-renderer-probe-resource-floor-route-negative-contract`.

## Native Adapter Selection Route

The command-line renderer probe now has a route for adapter-selection
telemetry. `renderer_probe_adapter_selection_route_smoke.py` runs the hardcoded
scene twice: once with `--adapter-backend null`, which verifies
`null_backend_requested=true`, `fallback_adapter_requested=false`, the Dawn
Null backend, command submission, and no pixel output; and once with
`--force-fallback-adapter`, which verifies `fallback_adapter_requested=true`,
`null_backend_requested=false`, early no-adapter failure, no command
submission, and no final software-golden eligibility. This route is native
probe-only and does not change `GpuSurface` or runtime-owned `core/view` code.

`renderer_probe_adapter_selection_route_contract.py` negative-tests fake probe
output for null-backend request drift, null-backend type drift, unexpected null
backend pixels, fallback request drift, fallback/null telemetry leakage, and
fallback command-submission leakage. CTest wires these as
`scene3d-renderer-probe-adapter-selection-route-contract` and
`scene3d-renderer-probe-adapter-selection-route-negative-contract`.
