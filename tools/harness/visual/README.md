# Visual Harness

The visual harness is the golden-file companion to the catalog/property
coverage harness in `tools/harness/`. The catalog harness answers whether an
entry reaches Pulp's bridge. This harness answers whether the resulting layout
or render output is correct under deterministic conditions.

Authoritative design: `planning/phase-b-visual-fidelity-spec.md` when the
private planning submodule is available. The public implementation keeps the
same shape without depending on the planning checkout.

## Phases

- **B.0 determinism setup:** lock Skia, HarfBuzz, ICU, and font inputs; provide
  a stable container/toolchain; prove a tiny SkPicture renders byte-identically
  twice when the locked Skia smoke dependency is installed.
- **B.1 layout snapshots:** run Yoga fixtures through `pulp-test-visual` and
  compare semantic JSON snapshots: DIP rects, z-order, clipping, measured text
  boxes, and hit regions.
- **Later phases:** add Skia PNG goldens for Canvas2D and view paint fixtures.

## Deterministic Inputs

The Day 1 locked stack is recorded in:

- `tools/deps/manifest.json`
- `external/skia-build/VERSION.md`
- `docs/reference/text-shaping.md`
- `tools/harness/visual/pins.py`
- `.github/workflows/visual-harness.yml`

The initial optional Skia smoke uses `skia-python==144.0.post2`, matching the
Skia 144 milestone, because fresh source worktrees can contain only
`external/skia-build` headers and metadata without platform static libraries.
Set `PULP_VISUAL_REQUIRE_SKIA=1` in CI/container environments so the smoke
fails instead of skipping when the locked Skia dependency is missing.

## Interaction And Capture

When a visual fixture needs interaction, prefer Pulp's in-process view hooks
over platform UI automation: `View::simulate_click`, `simulate_drag`, and
`simulate_hover` route through the same hit-test and pointer-event code used by
the hosts. The existing `examples/ui-preview` automation path is the current
reference for before/click/after capture.

For raster capture, prefer `pulp::view::render_to_png` / `render_to_file` for
headless view-tree screenshots, or `WindowHost::capture_png()` when a live
host surface is required. Apple platforms have a built-in screenshot backend;
Linux, Windows, and Android need a host-registered provider via
`set_screenshot_provider()` and should report an explicit skip or failure when
no provider is installed.

## Local Commands

```bash
python3 -m pytest tools/harness/visual/tests/
```

If `skia-python==144.0.post2` is not installed, the SkPicture render smoke
skips with an explicit reason. The pin and font integrity tests still run.

Build the semantic Yoga snapshot binary and verify checked-in B.1 goldens:

```bash
cmake --build build --target pulp-test-visual
pulp harness visual --verify --all
```

When a verification failure needs inspection, write actual captures beside the
normal build outputs:

```bash
pulp harness visual --verify --all --actuals-dir build/visual-actuals
```

The runner writes failed actuals to
`build/visual-actuals/<surface>/<fixture>.<json|png>`. Semantic JSON fixtures
use the tolerance-aware differ; raster PNG fixtures use exact-byte comparison
on the canonical macOS arm64 lane.

Regenerate one golden after an intentional layout contract change:

```bash
pulp harness visual --generate --surface=yoga --entry=yoga/box-sizing
```

`pulp harness coverage --surface=yoga --json` includes two validation
accounting blocks. `validation_routes` counts supported catalog entries with a
typed validation route, legacy test reference, or explicit exclusion.
`visual_coverage` counts supported entries whose typed runtime fixture refs
resolve to checked-in semantic or raster goldens; `visual_pass` remains as a
temporary compatibility alias. Runtime refs use prefixes such as
`semantic:<fixture-id>`, `visual:<fixture-id>`, `dom:<fixture-id>`, and
`behavior:<fixture-id>`. Non-runtime routes use `unit:<test-id>` or
`cannot-validate:<tracking-id>`.

To run the smoke in the stable Linux container once Docker is available:

```bash
tools/harness/visual/docker-build.sh
docker run --rm -v "$PWD:/workspace" pulp-visual-harness
```

The wrapper uses `linux/amd64` by default because the pinned smoke archive is
the Skia `linux-x64` release. It also exports/reuses a local buildx cache at
`~/.cache/pulp/visual-harness/buildx` by default, while the Dockerfile keeps
BuildKit cache mounts for apt packages, the downloaded Skia zip, and pip
wheels. Override with `PULP_VISUAL_IMAGE`, `PULP_VISUAL_DOCKER_PLATFORM`, or
`PULP_VISUAL_DOCKER_CACHE` when validating on shared SSH machines.

On GitHub-hosted Ubuntu, create a `docker-container` Buildx builder before
running the wrapper. The hosted image's default `docker` driver rejects local
cache export unless containerd image storage is enabled.

When macOS rendering is the product risk, run the local pytest smoke on
arm64-darwin as well. The Docker smoke proves the locked dependency recipe and
fresh-worktree behavior; the local lane proves the platform that will own the
canonical raster-golden gate.

The GitHub `macOS local smoke` job reads `PULP_LOCAL_MACOS_RUNS_ON_JSON` and
falls back to hosted `macos-15` only when that repo variable is unset. In Pulp's
normal CI config it should resolve to the local self-hosted `sanitizer` runner.
That local runner path uses the installed `python3.12` plus a per-checkout venv
for the locked `skia-python` wheel; it does not use `actions/setup-python`,
whose hosted-runner toolcache defaults are not writable on the persistent Mac.

The canonical raster-golden gate for future PNG tests is the arm64-darwin
software/raster lane. The Docker image is the reproducible dependency
environment for smoke and developer parity, not a replacement for that lane.
