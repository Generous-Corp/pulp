# UI Preview

**Category**: experimental
**Type**: Standalone application (not a plugin)
**Path**: `examples/ui-preview/`

## Summary

A standalone application for testing the view/widget system, imported UI scripts,
the inspector overlay, and the GPU rendering pipeline without building a full
plugin. With no script it opens a small built-in widget demo; with `--script` it
loads an imported JavaScript/JSX bundle through the `WidgetBridge` scripting
layer.

## What It Demonstrates

- The live rendering pipeline: script or built-in demo -> View tree -> layout -> paint -> GPU window
- `ScriptEngine` (QuickJS) integration for loading imported JavaScript UI bundles
- `WidgetBridge` binding JavaScript widget creation functions to the View hierarchy
- `StateStore` parameter setup outside of a plugin context
- `WindowHost` for native window creation and event loop
- In-canvas inspector overlay plus floating inspector window wiring
- Headless capture and diagnostics through `--screenshot`, `--view-tree-out`, `--label-audit`, and `--font-probe`
- Optional zero-copy benchmark mode when Pulp is configured with `PULP_BENCHMARK=ON`
- Dark theme application via `Theme::dark()`
- Flexbox layout configuration (`FlexDirection::column`, padding, gap)
- Built-in widget types: labels, knobs, toggles, faders, and scroll views

## Supported Formats

This is not a plugin. It builds as the native standalone `pulp-ui-preview`
target when the desktop UI/GPU dependencies are available.

## Supported Platforms

| Platform | Supported |
|----------|-----------|
| macOS | Yes, desktop only with GPU and Skia enabled |
| iOS | No |
| Windows | No (target is Apple-desktop gated in CMake) |
| Linux | No (target is Apple-desktop gated in CMake) |

## Key Files

| File | Purpose |
|------|---------|
| `main.cpp` | Application entry point: creates state, preview root, scripting bridge, inspector wiring, headless diagnostics, and WindowHost |
| `design_viewport_probe.hpp` | Header-only imported-design viewport resolver used by the live preview path |
| `CMakeLists.txt` | Build configuration; gates the target to Apple desktop with GPU, Skia, and `pulp::inspect` available |
| `test/test_ui_preview_viewport.cpp` | Headless tests for the imported-design viewport probe |
| `test/test_bench_integration.cpp` | Optional benchmark-mode integration test when `PULP_BENCHMARK` and `pulp-ui-preview` are built |

## Benchmark Mode

Benchmark mode is available when Pulp is configured with
`-DPULP_BENCHMARK=ON` and the `pulp-ui-preview` target is built. It runs a
headless, frame-paced visualization loop and writes the JSON schema consumed by
`tools/scripts/bench_diff.py`.

```bash
cmake -S . -B build-bench -DCMAKE_BUILD_TYPE=Release -DPULP_BENCHMARK=ON
cmake --build build-bench --target pulp-ui-preview pulp-test-bench-integration
./build-bench/examples/ui-preview/pulp-ui-preview \
  --benchmark-seconds=10 \
  --widget=oscilloscope \
  --output=/tmp/pulp-ui-preview-bench.json \
  --target-fps=60
```

`--benchmark-seconds=<n>` enables benchmark mode and sets the run duration.
`--output=<path>` is required when benchmark mode is enabled. `--widget=<name>`
defaults to `oscilloscope`, and `--target-fps=<n>` defaults to `60`.

Compare two captured runs with:

```bash
tools/scripts/bench_diff.py baseline.json /tmp/pulp-ui-preview-bench.json
```

## Known Limitations

- macOS desktop only. The CMakeLists.txt requires Apple desktop, GPU enabled,
  Skia available, and `pulp::inspect` present.
- It intentionally does not link audio or plugin-format libraries; it validates
  the UI stack in isolation.
- End-to-end live-window behavior is still primarily validated by launching the
  app or by desktop automation, but the imported-design viewport resolver and
  benchmark JSON contract have automated coverage.
- The executable is a developer validation host, not a stable application API.

## Related Examples

- [PulpGain](example-pulp-gain.html) -- the parameters used in this preview (Gain, Mix, Bypass) mirror PulpGain's parameter set
- [Showcase](example-showcase.html) -- JavaScript UI bundle that can be loaded through `pulp-ui-preview --script`
