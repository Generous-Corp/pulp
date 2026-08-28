# GPU Rendering Validation Checklist

Status of GPU rendering verification across platforms and configurations.

## Start With the Bounded Health Probe

Run the installed diagnostic before inspecting build files or opening a live
window:

```bash
pulp doctor gpu --json
```

The command performs a known offscreen draw plus pixel readback and a known
compute submission plus mapped-result verification. Use
`pulp doctor gpu --no-render --json` only for inventory/preflight when device
acquisition itself must be avoided; it performs no active render or compute
work and returns unverified. Both commands work outside a source checkout.
Agents can request the same typed evidence with the
`pulp_gpu_doctor` MCP tool.

Interpret the exit status with the JSON evidence:

- `0` means all required real-work proofs passed and at least one required
  probe reported authentic adapter identity.
- `1` means the work completed but a measured assertion failed.
- `2` means a requested stage was unavailable or could not be verified.

Do not turn exit 2 into a skipped pass. Likewise, do not infer a discrete or
hardware adapter from a Metal, D3D12, Vulkan, or other backend label. Trust only
the adapter identity returned by Dawn; a null/software adapter is useful for
negative coverage but is never hardware proof. Identities are per probe and do
not claim Renderer3D, HeadlessSurface, and GpuCompute acquired the same device.

If the diagnostic reports unavailable or unverified, follow the remediation in
the result, then use the [Skia GPU build skill](../../.agents/skills/skia-gpu-build/SKILL.md)
to inspect bundle discovery and ABI details. If it reports a completed failure,
preserve the JSON result and reproduce with the focused render/compute tests
before moving to a live application.

## Verified (Real Hardware)

| Platform | Backend | Surface | Rendering | Status |
|----------|---------|---------|-----------|--------|
| macOS (Apple Silicon) | Metal | CAMetalLayer (NSView) | Skia Graphite | **Verified** — GPU demo runs at 60fps |
| macOS (Apple Silicon) | Metal | CAMetalLayer (PluginViewHost) | Skia Graphite | **Verified** — DAW-embedded rendering |

## Implemented (Awaiting Hardware Validation)

| Platform | Backend | Surface | Rendering | Status |
|----------|---------|---------|-----------|--------|
| iOS | Metal | CAMetalLayer (UIView) | Skia Graphite | **Implemented** — IOSGpuWindowHost + IOSGpuPluginViewHost |
| Windows | D3D12 | HWND (SDL3) | Skia Graphite | **Implemented** — SDL3 HWND extraction + Dawn D3D12 |
| Linux/X11 | Vulkan | X11 Window (SDL3) | Skia Graphite | **Implemented** — SDL3 X11 extraction + Dawn Vulkan |
| Android | Vulkan | ANativeWindow (SurfaceView) | Skia Graphite | **Implemented** — ANativeWindow extraction + Dawn Vulkan |

## Architecture

```
Platform Window (NSView/UIView/HWND/SurfaceView/SDL3)
    │
    ▼
Native Surface Handle (CAMetalLayer*/HWND/ANativeWindow/X11 Window)
    │
    ▼
GpuSurface (Dawn/WebGPU)
    ├── Metal backend (macOS/iOS)
    ├── D3D12 backend (Windows)
    └── Vulkan backend (Linux)
    │
    ▼
SkiaSurface (Skia Graphite)
    │
    ▼
SkCanvas → View tree painting
```

## Render Loop

| Platform | Mechanism | Target FPS |
|----------|-----------|------------|
| macOS | CVDisplayLink → main queue dispatch | Display refresh (60-120Hz) |
| iOS | CADisplayLink | Display refresh (60-120Hz) |
| Android | AChoreographer | Display refresh |
| Windows | DwmFlush, with timer fallback if DWM is unavailable | Display refresh or 60Hz fallback |
| Linux | Timer fallback until native present-sync is wired | 60Hz |
| WASM | requestAnimationFrame | Display refresh |

## Known Limitations

- **Windows GPU**: `DwmFlush` gives compositor-paced frames when DWM is available; headless or remote sessions degrade to the 60Hz timer fallback
- **Linux GPU**: X11 surface creation is wired, but frame pacing is still the 60Hz timer fallback
- **iOS GPU**: CADisplayLink frame pacing exists, but device runtime validation is still pending
- **Linux Wayland**: SDL3 can extract Wayland handles, but `GpuSurface` presentation consumes X11 handles only today
- **WASM**: WebGPU support depends on browser (Chrome 113+, Firefox 120+)

## Test Coverage

- 13 cross-platform render tests (GpuSurface + SkiaSurface)
- GPU demo validates continuous animation, vector drawing, resize
- Headless tests verify surface creation and texture lifecycle
- `pulp doctor gpu` verifies bounded render/readback and compute/map work using
  the same public rendering primitives an installed consumer uses
- `pulp gpu probe --recipe <id> --artifacts <dir>` localizes wrong values and
  pixels with deterministic inputs, independent CPU/content oracles, authentic
  adapter identity, and bounded hash-declared artifacts. Run the matching
  `--negative-control` path to prove the oracle detects a real seeded mutation;
  its expected exit status is 1 with typed failure evidence.
