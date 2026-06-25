# Renderer3D Module Map

This map is the extraction contract for splitting `core/render/src/renderer3d.cpp`.
The file still owns the native Renderer3D proof path in one 3,474-line
translation unit, with shared CPU value types now isolated in the 182-line
private `core/render/src/renderer3d_internal.hpp` header. Remaining work in the
translation unit includes SceneData-to-CPU draw normalization, material and
texture decoding, light/camera/animation metadata projection, Dawn
device/target setup, WGSL shader and pipeline construction, per-primitive GPU
upload, render-pass submission, readback, PNG encoding, and result-flag
reporting.

Future extraction PRs should preserve the contracts below or update this file,
the focused renderer tests, and `tools/scripts/hotspot_size_guard.json` in the
same change.

## Current Source Map

| Region | Current lines | Owns today | Extraction owner |
| --- | --- | --- | --- |
| Public facade | `core/render/include/pulp/render/renderer3d.hpp` | Render configs, render result flags, backend preference enum, and the two public `Renderer3D` entry points. | Keep public unless a later API review intentionally changes the surface. |
| CPU draw types | `renderer3d_internal.hpp` lines 11-180 | `SceneVertex`, `CpuTexture`, `CpuPrimitive`, pipeline key structs, CPU light/camera structs, normalization metadata, and deferred-feature structs. | Extracted into the private shared header; focused modules should include it instead of re-declaring these records. |
| Scene graph metadata | `renderer3d.cpp` lines 35-638 | Node transform classification, camera/light feature detection, transformed light/camera selection, initial animation pose, unsupported-feature flags, and adapter-info recording. | `renderer3d_scene.{hpp,cpp}`. |
| Material and texture normalization | `renderer3d.cpp` lines 639-1488 | Texture decode/fallbacks, sampler conversion, UV transforms, tangent derivation, SceneData primitive collection, material flag projection, texture route support, normalization bounds, alpha-sort depth, and render-packet error handling. | `renderer3d_materials.{hpp,cpp}` for material/texture rules and `renderer3d_scene.{hpp,cpp}` for render-packet traversal/normalization. |
| Backend preference adapter | `renderer3d.cpp` lines 1489-1502 | Public `Renderer3DAdapterBackendPreference` to `GpuSurface::AdapterBackendPreference` mapping. | `renderer3d_gpu_upload.{hpp,cpp}` or a small internal helper in `renderer3d_internal.hpp`. |
| Hardcoded cube proof | `renderer3d.cpp` lines 1503-1998 | Native Dawn smoke path, fixed cube geometry, hardcoded WGSL, offscreen targets, buffer/texture upload, pipeline creation, command submission, readback, PNG encoding, and structural result checks. | Leave in `renderer3d.cpp` until the SceneData path has focused helpers, then route through shared upload/pipeline/readback helpers. |
| SceneData facade and result projection | `renderer3d.cpp` lines 1999-2289 | Public `render_scene_data` orchestration, render-result metadata aggregation, backend initialization, target allocation, and handoff into upload/pipeline/readback blocks. | Thin shell in `renderer3d.cpp` plus `renderer3d_scene_result.{hpp,cpp}` only if result aggregation remains large after scene/material extraction. |
| SceneData shader and layout contract | `renderer3d.cpp` lines 2290-2656 | Uniform ABI, WGSL shader source, vertex attribute layout, depth state, and GPU primitive holder shape. | `renderer3d_pipeline.{hpp,cpp}`. |
| SceneData GPU upload | `renderer3d.cpp` lines 2657-3285 | Per-primitive uniform, vertex/index buffer, texture, texture view, sampler, bind group, and pipeline-cache construction. | `renderer3d_gpu_upload.{hpp,cpp}` for resources and `renderer3d_pipeline.{hpp,cpp}` for pipeline-cache decisions. |
| SceneData draw/readback | `renderer3d.cpp` lines 3286-3474 | Alpha-blend ordering, render pass, command encoder, texture-to-buffer copy, map/poll loop, compact-row copy, image statistics, PNG encoding, and final success/error synthesis. | `renderer3d_readback.{hpp,cpp}` for readback/statistics/PNG and `renderer3d_pipeline.{hpp,cpp}` for draw ordering if it is coupled to pipeline state. |
| Build/test wiring | `core/render/CMakeLists.txt`, `test/CMakeLists.txt` | Scene3D render build gates, probe executable, renderer3d test suite, probe route contracts, golden manifest checks, and native renderer boundary checks. | Update alongside each code move. |

## Target Modules

| Module | Owns | Must not own |
| --- | --- | --- |
| `renderer3d_internal.hpp` | Shared internal value types and narrow helper declarations needed by more than one Renderer3D module: CPU draw structs, GPU primitive resource structs if shared later, and result/stat helper declarations. | Public Renderer3D API, test-only helpers, scene parser types beyond `pulp::scene::SceneData`/render-packet data already consumed by the renderer, standalone/probe CLI behavior, or unnecessary Dawn/Skia includes. |
| `renderer3d_scene.{hpp,cpp}` | SceneData traversal, render-packet consumption, world transform normalization, light/camera/animation/deferred-feature metadata, scene bounds/scale, alpha-sort depth inputs, and the conversion from SceneData primitives to CPU draw records. | Dawn/WebGPU resource creation, shader strings, render pipelines, PNG encoding, probe CLI parsing, glTF loading, or public SceneData parser ownership. |
| `renderer3d_materials.{hpp,cpp}` | CPU material projection: texture decoding via Skia, fallback textures, sampler/filter conversion policy, UV transforms, tangent derivation, base-color/PBR/normal/occlusion/emissive feature flags, and material-related deferred-route flags. | Scene graph traversal, Dawn texture allocation, WGSL shader ownership, render-pass submission, public material parser data structures, or filesystem fixture loading. |
| `renderer3d_gpu_upload.{hpp,cpp}` | Dawn device/queue/instance extraction, offscreen target allocation helpers, CPU-to-GPU buffer and texture upload, texture view and sampler creation, bind group creation, and backend preference mapping. | CPU SceneData interpretation, shader source, pipeline-cache policy, render-result feature aggregation not tied to upload completion, command submission/readback polling, or PNG encoding. |
| `renderer3d_pipeline.{hpp,cpp}` | WGSL shader source, uniform ABI, vertex attribute layout, depth/blend/culling state, pipeline-cache key/policy, bind group layout assumptions, alpha-blend depth-write policy, and draw ordering rules that depend on pipeline state. | Texture decoding, SceneData traversal, backend/device initialization, readback row compaction, PNG encoding, probe CLI parsing, or public API definitions. |
| `renderer3d_readback.{hpp,cpp}` | Render pass submission helpers, color-target copy into readback buffers, map/poll timeout behavior, padded-row compaction, distinct-color/non-transparent statistics, PNG encoding through `HeadlessSurface`, and final structural-success checks. | SceneData conversion, material feature flags, sampler/filter policy, pipeline creation, public Renderer3D configs, or filesystem output from `renderer3d_probe.cpp`. |
| `renderer3d.cpp` | Public facade orchestration for `render_hardcoded_textured_cube` and `render_scene_data`, feature-gate fallbacks when Skia/WebGPU are unavailable, result initialization, and sequencing across focused internal modules. | New business rules that can live in a focused module, raw shader strings, bulk resource-upload loops, scene/material conversion internals, or readback/statistics implementation. |

## Dependency Direction

Keep the split acyclic:

```text
renderer3d.cpp
  -> renderer3d_scene
      -> renderer3d_materials
      -> renderer3d_internal
  -> renderer3d_gpu_upload
      -> renderer3d_pipeline
      -> renderer3d_internal
  -> renderer3d_pipeline
      -> renderer3d_internal
  -> renderer3d_readback
      -> renderer3d_internal
```

Rules:

- `renderer3d_scene` may depend on `renderer3d_materials` because CPU draw
  records need material projection while traversing render-packet primitives.
- `renderer3d_materials` must not depend on `renderer3d_scene`; feed it
  primitive/material/texture/sampler inputs and return CPU material state.
- `renderer3d_pipeline` may know the GPU-facing layout of `SceneVertex` and
  `Uniforms`; it must not know how those values were derived from SceneData.
- GPU primitive holder structs shared by upload, pipeline, and draw/readback
  helpers belong in `renderer3d_internal.hpp`; focused modules own behavior
  around the holder, not duplicate holder definitions.
- `renderer3d_gpu_upload` may call pipeline helpers to build or reuse pipeline
  state; it must not create new shader strings or material policy.
- `renderer3d_readback` must accept already-created targets/resources and
  produce readback/stat/PNG outputs; it must not allocate scene textures or
  interpret SceneData.
- `core/scene` remains upstream of Renderer3D. Do not move render-packet,
  parser, sidecar, or glTF ownership into `core/render`.

## Boundary Rules

- Public API remains in `core/render/include/pulp/render/renderer3d.hpp`.
  Internal splits must not expose Dawn, Skia, or `pulp::scene` implementation
  details through the public header.
- Feature gates stay explicit. Code that requires both `PULP_HAS_SKIA` and
  `PULP_HAS_WEBGPU` must remain unavailable in CPU-only builds, with the same
  public fallback error strings unless tests are updated in the same PR.
- `Renderer3D::render_scene_data` consumes normalized `pulp::scene::SceneData`;
  it must not load glTF files, read sidecars, or depend on probe CLI arguments.
- `renderer3d_probe.cpp` remains a CLI adapter. It may call the public
  Renderer3D API but must not include internal Renderer3D module headers.
- Material/texture rules stay CPU-side until upload. Dawn texture allocation
  must not make decisions about glTF material semantics.
- Pipeline cache keys must stay derived from GPU state that affects pipeline
  creation, currently alpha blend and double sided. Adding material features to
  the key requires tests that prove cache count and route behavior.
- Readback timeout, row padding, PNG encoding, and structural result checks
  must stay shared between hardcoded-cube and SceneData rendering once the
  helpers exist.
- Lower the `core/render/src/renderer3d.cpp` ceiling in
  `tools/scripts/hotspot_size_guard.json` to the exact new line count in every
  extraction PR that shrinks it.

## Current Test Anchors

- `test/test_renderer3d_loading.cpp` covers hardcoded-cube rendering, adapter
  selection, parsed SceneData rendering, generated GLB rendering, fixture
  rendering, and Draco-related loader handoff behavior.
- `test/test_renderer3d_scene.cpp` covers scene traversal, node transforms,
  UV transforms, sampler downgrades, multi-primitive rendering, light handling,
  camera transforms/projections/aspect/depth metadata, and render-result flags.
- `test/test_renderer3d_materials.cpp` covers animation initial pose,
  unsupported-feature deferral, material extensions, unlit/alpha/vertex-color
  routes, normals/tangents, normal/metallic/roughness/occlusion/emissive
  texture routes, culling, blending, and sort behavior.
- `test/test_renderer3d_shared.hpp` owns the shared SceneData fixtures and is
  itself guarded as a hotspot; extraction PRs should avoid expanding it unless
  new behavior requires a reusable fixture.
- `test/CMakeLists.txt` wires `pulp-test-renderer3d`, the
  `pulp-renderer3d-probe` route contracts, golden manifest validation, public
  probe surface checks, fake-surface checks, and native renderer link-boundary
  checks.
- `tools/scene3d/verify_renderer_probe*.py` and route smoke scripts are the
  probe-level contract tests. Update them when public probe output changes, not
  for internal implementation-only moves.

## Extraction Order

1. Done: CPU value types and narrow shared declarations live in
   `renderer3d_internal.hpp`. The public header remains unchanged and CPU-only
   builds still use the same feature-gated fallback surface.
2. Move SceneData traversal and render-packet normalization into
   `renderer3d_scene.{hpp,cpp}` while keeping the current
   `test_renderer3d_scene.cpp` and render-packet contract tests unchanged.
3. Move material/texture helpers into `renderer3d_materials.{hpp,cpp}` with
   `test_renderer3d_materials.cpp` and probe route contracts as the regression
   corpus.
4. Move shared target/resource upload helpers into
   `renderer3d_gpu_upload.{hpp,cpp}`. Keep hardcoded-cube rendering green before
   sharing helpers with the SceneData path.
5. Move shader, uniform ABI, vertex layout, depth/blend/culling, and pipeline
   cache policy into `renderer3d_pipeline.{hpp,cpp}`. Add or update focused
   tests if the pipeline key expands beyond alpha blend and double sided.
6. Move readback, padded-row compaction, statistics, PNG encoding, and final
   structural validation into `renderer3d_readback.{hpp,cpp}` and then reuse
   it from both public render paths.
7. Leave `renderer3d.cpp` as the public orchestration shell and lower the
   hotspot guard ceiling after each shrink.

## Validation Gates

For documentation-only map changes:

- `git diff --check`
- `python3 tools/scripts/hotspot_size_guard.py --mode=report --base origin/main`
- Adversarial review against the diff and current Renderer3D source.

For any code extraction:

- Configure a Release build with `PULP_ENABLE_SCENE3D=ON` and the repo's
  normal Skia/WebGPU settings.
- Build `pulp-render`, `pulp-renderer3d-probe`, and `pulp-test-renderer3d`.
- Run `ctest --test-dir build -R "renderer3d|scene3d-renderer|scene3d-native-renderer" --output-on-failure`.
- Run `python3 tools/scripts/hotspot_size_guard.py --mode=report --base origin/main`.
- Run `git diff --check`.
- If the public Renderer3D or probe surface changes, run the probe public
  surface and route-inventory contracts in `test/CMakeLists.txt`.

## Non-Goals

- Do not add a new SceneData parser, glTF loader, material schema, or sidecar
  owner under `core/render`.
- Do not move `renderer3d_probe.cpp` into the internal renderer modules.
- Do not change public result flags only to make extraction easier; those flags
  are part of the current test surface.
- Do not introduce a second readback/PNG path when the existing hardcoded-cube
  and SceneData paths can converge on one helper.
- Do not raise hotspot ceilings for Renderer3D split files. Extraction PRs
  should reduce `renderer3d.cpp` and keep new focused files below the
  new-file warning limit unless a reviewed exception is documented.
