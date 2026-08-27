# GPU UX baseline for `pulp doctor gpu`

This is the A0 source and ownership baseline for the GPU-health work. It records facts, not a runtime pass claim.

## Revisions and host

- Pulp audit revision: `6c776d5e1d835f17101a0388d77a932edb7d6236` (`origin/main` when the execution worktree was created).
- Forge local audit revision: `36ed776426a137397f5ad02187d3bb00bf49c661`.
  That checkout has unresolved `UU`/`DU` paths and is no-touch evidence, not a
  canary source.
- Forge canary revision: `0750a88dea3af7fca927a8c02887e071109407ae`
  (`Generous-Corp/forge` `origin/main` at audit time), pinned to Pulp
  `d87bce52804eb99275d146fe0f0f962eee3b9997` before this work. A later canary
  must use a fresh worktree at that exact Forge revision and repin to the exact
  installed A1 Pulp commit; it must not reuse the conflicted local checkout.
- Host: macOS 26.6.2 (25G83), Apple clang 21.0.0 (`clang-2100.1.1.101`).
- Adapter identity and live GPU verdict: **unverified at A0**. Source inspection cannot authenticate the adapter or prove submitted pixels. The v1 contract intentionally prevents that absence from becoming a pass.

## Existing route outcomes

| Route | A0 outcome | Evidence at the audited Pulp revision |
| --- | --- | --- |
| `Renderer3D` | Reuse in Pulp | It already exposes native Dawn cube and normalized `SceneData` entry points (`renderer3d.hpp:141-151`). A doctor probe should call these entry points and apply a content floor to their returned pixels. |
| `GpuCompute` | Reuse and strengthen in Pulp | Standalone initialization requests an adapter, rejects Null/Undefined backends, and requests a device (`gpu_compute.cpp:1101-1180`). Capabilities expose backend/vendor only after initialization (`gpu_compute.cpp:2642-2679`). |
| Invalid WGSL | Preserve as an explicit asynchronous failure baseline | Pipeline construction presently checks returned handles (`gpu_compute.cpp:3642-3668`), while the uncaptured-error callback logs asynchronously (`gpu_compute.cpp:1153-1157`). A successful-looking creation return is therefore not compile proof; the seeded `fail-async-invalid-wgsl.json` fixture requires the callback failure to win. |
| `HeadlessSurface` | Consume only; do not modify in Pulp | It already owns offscreen creation, deterministic clear, readback, and fingerprint helpers (`headless_surface.hpp:40-105`). The ownership projection routes this path to Vellum as `initial-cut-transfer`. GPU doctor may call the projected API but must not originate generic changes here. |
| Forge | Consume Pulp's stable result | Forge should invoke the Pulp-facing command/MCP result and render recommendations. It should not duplicate Dawn probing or infer health from process exit alone. |

The routing helper at route-set SHA-256 `5bb857c44cdba4cf4e2d584b49ea197960e9b1b490703ce3500c649dc9995028` reports Pulp ownership (`repository-default`) for the GPU-health CLI/provider, MCP adapter, `Renderer3D`, `GpuCompute`, contract, and trace-analysis skill. It reports Vellum ownership (`initial-cut-transfer`) for `core/render/include/pulp/render/headless_surface.hpp`. Because the combined candidate set spans both owners, its aggregate decision is `coordinated`.

The exact A1-A5/A2T candidate routing invocation at the audited revision
returned the following replayable rows. All paths were qualified as
`Generous-Corp/pulp`; `cell_roles` was empty for every row and the expansion was
`full-design-import-render-v1`.

| Package | Candidate path | Owner | Route kind |
| --- | --- | --- | --- |
| A1 | `tools/cli/gpu_health/src/gpu_health.cpp` | `Generous-Corp/pulp` | `repository-default` |
| A1 | `tools/mcp/mcp_gpu_tools.cpp` | `Generous-Corp/pulp` | `repository-default` |
| A1/A2 | `core/render/include/pulp/render/renderer3d.hpp` | `Generous-Corp/pulp` | `repository-default` |
| A1/A2 | `core/render/include/pulp/render/gpu_compute.hpp` | `Generous-Corp/pulp` | `repository-default` |
| A1 | `core/render/include/pulp/render/headless_surface.hpp` | `Generous-Corp/vellum` | `initial-cut-transfer` |
| A2 | `tools/cli/cmd_gpu.cpp` | `Generous-Corp/pulp` | `repository-default` |
| A2T | `core/runtime/src/trace.cpp` | `Generous-Corp/pulp` | `repository-default` |
| A2T | `.agents/skills/trace-analysis/SKILL.md` | `Generous-Corp/pulp` | `repository-default` |
| A2T | `.agents/skills/trace-sql/SKILL.md` | `Generous-Corp/pulp` | `repository-default` |
| A3 | `core/view/src/host_frame_pump.cpp` | `Generous-Corp/pulp` | `repository-default` |
| A3 | `core/view/src/view_paint.cpp` | `Generous-Corp/pulp` | `repository-default` |
| A4 | `core/view/platform/mac/window_host_mac.mm` | `Generous-Corp/pulp` | `repository-default` |
| A4 | `core/render/include/pulp/render/skia_surface.hpp` | `Generous-Corp/vellum` | `initial-cut-transfer` |
| A5 | `docs/status/gpu-recipes.yaml` | `Generous-Corp/pulp` | `repository-default` |

This result authorizes Pulp-owned adapters, trace/product seams, experiments,
and workflow projection. It does not authorize changing either transferred
generic surface. Any missing HeadlessSurface or SkiaSurface primitive is a
Vellum package, with Pulp limited to consuming the accepted interface.

## Contract baseline

`gpu-health-result-v1` separates four verdicts that must never collapse:

- `pass`: authentic evidence reached readback, produced pixels, and passed a deterministic content floor.
- `fail`: a requested stage ran and produced negative evidence, including an asynchronous Dawn validation or device-loss event.
- `unavailable`: the requested probe could not run, such as no usable adapter or a build without the required GPU feature.
- `unverified`: the caller did not request the proving route, or identity/evidence was insufficient to make a claim.

The result records globally ordered per-stage events, adapter identity status/classification, measurements, health state, and recommendations. Adapter identities are per probe: v1 makes no same-device or correlation claim across independently acquired Renderer3D, HeadlessSurface, and GpuCompute devices. A top-level pass requires authentic identity from at least one required probe plus required real render/readback/content proof; the identity status on every other probe remains visible and is never upgraded by association. A software adapter may pass pixel health, but it is reported as software; a Null adapter can never pass. JSON Schema closes the transport shape, while the C++ and Python semantic validators enforce ordering, roll-up precedence, identity authenticity, device-loss agreement, and pixel-proof requirements.

## Acceptance boundary for A1

A1 is successful only when the CLI returns the same typed result in human and JSON modes; `--no-render` returns `unverified`; missing adapters return `unavailable`; Null is never treated as healthy; rendering requires non-transparent/distinct-color content plus readback/fingerprint evidence; and compute requires a deterministic output oracle. The A0 asynchronous-invalid-WGSL source and result fixture remain a pinned `fail` baseline; A1 does not claim to execute or fix that asynchronous compiler path. HeadlessSurface remains a consume-only dependency so the later Vellum adoption does not require redesigning the result contract or Forge integration.

## A1 local value evidence

The first implementation proof ran on the A0 host from this worktree with
`PULP_ENABLE_SCENE3D=ON`, tests and examples disabled, and the dedicated
`pulp-gpu-health-scene3d-acceptance` target. The real result was `pass`: all
three required probes completed, Renderer3D returned 4,096 non-transparent
pixels and 424 distinct colors, HeadlessSurface returned 4,096 pixels with the
expected clear-color fingerprint, and GpuCompute returned the independent
3-4-5 magnitude oracle on an authentic Apple M3 Ultra Metal adapter. The same
binary then ran the seeded post-readback content mismatch and rejected it. This
is local-host evidence, not a claim about untested platform adapters.

The closed contract suite accepted all eight fixtures and rejected 101 schema
or semantic mutations, including diagnostic-code/stage/verdict drift. Native
CLI, Rust delegation, source-layout MCP from an unrelated working directory,
and clean-prefix installed `pulp-cpp`, `pulp`, and `pulp-mcp` fronts preserved
the same typed no-render result and exit 2. Enabling Scene3D for the macOS
release CLI build changed the local `pulp-cpp` image from 56,709,976 to
56,865,288 bytes (+155,312 bytes); its dynamic dependency list added no
non-system runtime beyond the already-required `libwgpu_native.dylib`. The SDK
release configuration remains Scene3D-off because Pulp does not currently
export the scene library. These figures are a build-local regression sentinel,
not a cross-platform package-size budget.

The required Forge Modular exact-SHA canary remains the final downstream value
gate. Record it against the immutable Pulp implementation commit after that
commit exists; do not substitute this source-worktree proof for the Forge
result.
