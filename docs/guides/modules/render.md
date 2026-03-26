# Render Module

The render module manages GPU surfaces and the Skia Graphite rendering context. It connects Dawn (WebGPU) to Skia for hardware-accelerated 2D rendering.

**Status**: experimental
**Dependencies**: runtime, canvas
**Headers**: `pulp/render/gpu_surface.hpp`, `pulp/render/skia_surface.hpp`

## Architecture

The rendering stack layers:

```
View → Canvas (abstract) → SkiaCanvas → Skia Graphite → Dawn (WebGPU) → Metal/D3D12/Vulkan
```

1. **Dawn** provides the WebGPU device and surface (GPU access)
2. **Skia Graphite** creates a recording context on top of Dawn
3. **SkiaSurface** wires Dawn's device to Skia's Graphite context
4. **SkiaCanvas** implements the Canvas interface using Skia's SkCanvas

## GpuSurface

Abstract GPU surface representing a renderable target. The Dawn implementation handles device creation and frame lifecycle.

```cpp
#include <pulp/render/gpu_surface.hpp>

auto surface = GpuSurface::create_dawn();
surface->initialize({.width = 800, .height = 600, .vsync = true});

// Render loop
while (running) {
    if (surface->begin_frame()) {
        // ... draw with Skia ...
        surface->end_frame();
    }
}
```

### Frame Lifecycle

1. `begin_frame()` — acquires the next GPU texture
2. Draw commands are recorded via Skia's Graphite Recorder
3. `end_frame()` — submits the recording and presents

## SkiaSurface

Connects a Dawn GpuSurface to Skia Graphite for 2D rendering:

```cpp
#include <pulp/render/skia_surface.hpp>

SkiaSurface skia;
skia.initialize({.gpu_surface = gpu_surface.get()});

// Each frame:
if (auto canvas = skia.begin_recording()) {
    SkiaCanvas pulp_canvas(canvas);
    root_view.paint_all(pulp_canvas);
    skia.end_recording();
}
```

## GPU Backends

Dawn supports multiple backends, selected at build time:

| Platform | Backend | Status |
|----------|---------|--------|
| macOS | Metal | experimental |
| Windows | D3D12 | planned |
| Linux | Vulkan | planned |
| Web | WebGPU | planned |

The pre-built Skia Graphite binaries (in `external/skia-build/`) include Dawn for all platforms.

## When to Use

The render module is optional. Most of the framework works without it:

- **With render**: GPU-accelerated UI, Skia drawing, WebGPU effects
- **Without render**: CoreGraphics canvas (macOS), headless testing, CLI tools

Plugins can be built, tested, and shipped without linking the render module. The format and state subsystems have no dependency on it.
