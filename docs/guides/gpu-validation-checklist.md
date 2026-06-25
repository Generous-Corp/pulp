# GPU Rendering Validation Checklist

Status of GPU rendering verification across platforms and configurations.

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
