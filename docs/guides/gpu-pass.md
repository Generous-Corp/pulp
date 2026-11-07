# Your First WGSL Pass in a View

This guide draws a custom WGSL pass on Pulp's shared GPU device and composites
the result into a `View`. It uses the typed API in
`<pulp/render/gpu_pass.hpp>`: no raw `void*` handles and no Dawn headers in
your code.

Use this path when a fragment shader over a rectangle (`Canvas::draw_with_sksl`,
see [Custom Shaders](../reference/shaders.md)) is not enough: you need your
own vertex stage, several passes, depth, or an intermediate texture.

## The pieces

| Type | What it is |
|---|---|
| `GpuDevice` | Typed handle to the device a `GpuSurface` owns. `GpuDevice::from_surface(surface)` |
| `GpuShaderModule` | Compiled WGSL. A compile error returns line/column diagnostics |
| `GpuRenderPipeline`, `GpuComputePipeline` | Pipelines with automatic (`layout: auto`) bind group layouts |
| `GpuBuffer`, `GpuTexture`, `GpuSampler`, `GpuBindGroup` | Resources and their bindings |
| `GpuTarget` | Offscreen color texture (plus optional depth) with CPU readback |
| `GpuFrame` | One command encoder: open passes, record, `submit()` |
| `GpuRenderPass`, `GpuComputePass` | An open pass inside a frame |
| `GpuSubmission` | Non-blocking completion status of a submitted frame |
| `draw_gpu_texture()` | Composites a texture through the Skia canvas |

Every call that can fail returns `GpuResult<T>` (or `GpuStatus`) carrying a
`GpuError` with a kind (`shader_compilation`, `validation`, `device_lost`, ...)
and a message.

## 1. Write the shader

Put the WGSL in a file next to your sources, `shaders/glow.wgsl`:

```wgsl
struct Params {
    color: vec4f,
    time: f32,
};
@group(0) @binding(0) var<uniform> params: Params;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
    // One triangle that covers the whole target.
    let corners = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    return vec4f(corners[index], 0.0, 1.0);
}

@fragment
fn fs_main(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let pulse = 0.5 + 0.5 * sin(params.time * 3.0 + position.x * 0.05);
    // Premultiplied: the canvas composites premultiplied alpha.
    return vec4f(params.color.rgb * pulse, 1.0);
}
```

Embed it, and validate it at build time:

```cmake
pulp_add_wgsl_shaders(MyPlugin
    FILES shaders/glow.wgsl
    NAMESPACE my_plugin::shaders)
```

That generates `glow_wgsl.hpp` with
`inline constexpr std::string_view my_plugin::shaders::glow_wgsl`. A shader that
does not compile fails the build with `glow.wgsl:12:5: error: ...`. See
[`pulp_add_wgsl_shaders`](../reference/cmake.md#pulp_add_wgsl_shaders) for when
validation runs.

## 2. Create the device objects once

The host owns the `GpuSurface`: `WindowHost::gpu_surface()` in an app,
`PluginViewHost::gpu_surface()` in a plug-in editor. Build everything that does
not change per frame when the surface becomes available, not in `paint()`.

```cpp
#include <pulp/render/gpu_pass.hpp>
#include "glow_wgsl.hpp"

using namespace pulp::render;

struct GlowGpu {
    GpuDevice device;
    GpuRenderPipeline pipeline;
    GpuBuffer params;
    GpuBindGroup bindings;
    GpuTarget target;
};

GpuResult<GlowGpu> make_glow(GpuSurface& surface, uint32_t width, uint32_t height) {
    GlowGpu gpu;
    auto device = GpuDevice::from_surface(surface);
    if (!device) return runtime::Err(device.error());
    gpu.device = device.value();

    auto shader = gpu.device.create_shader_module(my_plugin::shaders::glow_wgsl, "glow.wgsl");
    if (!shader) return runtime::Err(shader.error());   // error().describe() lists line:column

    GpuRenderPipelineDesc desc;
    desc.module = shader.value();
    desc.color_targets = {{GpuFormat::rgba8_unorm, GpuBlend::replace}};
    auto pipeline = gpu.device.create_render_pipeline(desc);
    if (!pipeline) return runtime::Err(pipeline.error());
    gpu.pipeline = pipeline.value();

    auto params = gpu.device.create_buffer({.size = 32});       // uniform | copy_dst
    if (!params) return runtime::Err(params.error());
    gpu.params = params.value();

    auto bindings = gpu.device.create_bind_group(gpu.pipeline, 0, {GpuBinding::of(0, gpu.params)});
    if (!bindings) return runtime::Err(bindings.error());
    gpu.bindings = bindings.value();

    auto target = gpu.device.create_target({.width = width, .height = height});
    if (!target) return runtime::Err(target.error());
    gpu.target = target.value();
    return runtime::Ok(std::move(gpu));
}
```

`GpuDevice::capabilities()` reports the device's limits, its renderable and
storage formats and the sample counts each format allows.
`pick_tier(capabilities().facts)` reduces them to `none`, `basic`, `standard`
or `high`, which is a simple way to choose between a full and a reduced
effect. It is a pure function, so a test can cover that choice without a GPU.

## 3. Record, submit, composite

```cpp
void GlowView::paint(pulp::canvas::Canvas& canvas) {
    if (!gpu_) { paint_fallback(canvas); return; }

    struct { float color[4]; float time; float pad[3]; } params{{0.2f, 0.6f, 1.0f, 1.0f}, time_};
    gpu_->device.write_buffer(gpu_->params, 0, &params, sizeof(params));

    {
        GpuFrame frame(gpu_->device, "glow");
        auto pass = frame.render_pass(GpuRenderPassDesc::for_target(gpu_->target));
        pass.set_pipeline(gpu_->pipeline);
        pass.set_bind_group(0, gpu_->bindings);
        pass.draw(3);
        pass.end();
        if (auto submitted = frame.submit(); !submitted)
            pulp::runtime::log_error("glow: {}", submitted.error().describe());
    }

    const auto b = bounds();
    if (!draw_gpu_texture(canvas, gpu_->target.color(), 0, 0, b.width, b.height))
        paint_fallback(canvas);   // a CPU or recording canvas
}
```

## Rules that keep it correct

- **Submit before you composite.** `draw_gpu_texture()` refuses a texture that
  an unsubmitted `GpuFrame` writes. The shared queue runs submissions in
  order, and Skia submits the UI frame after `paint()` returns, so a frame
  submitted inside `paint()` is complete before Skia samples its output.
- **Do not draw on the canvas while a `GpuFrame` is open.** A frame opens
  WebGPU error scopes when it begins and closes them at `submit()`. Errors
  raised in between are reported as this frame's errors, which is what keeps
  a mistake in your pass from reaching the device's uncaptured-error handler
  (other users of the device, such as GPU audio, treat that handler as fatal
  to their own work).
- **One pass at a time.** End a pass before opening the next; a second open
  pass makes `submit()` fail and nothing from that frame is submitted.
- **Texture lifetime.** `draw_gpu_texture()` holds a reference to the texture
  until Skia releases the image, so dropping your handle right after the call
  is safe.
- **Surface lifetime.** Release the `GpuDevice` and every object made from it
  before the `GpuSurface` is destroyed. Rebuild them when the host recreates
  the surface.
- **Device loss.** Every call checks for a lost device and fails with
  `GpuError::Kind::device_lost`. Drop the objects and rebuild from the new
  surface.
- **Threading.** Create, record and submit on the thread that paints. Never
  touch this API from the audio thread.
- **Formats you can composite:** `rgba8_unorm`, `bgra8_unorm` and
  `rgba16_float`, single-sample. Resolve a multisampled pass into one of
  these with `GpuColorAttachment::resolve`.

## Reading pixels back

`GpuTarget::read_pixels()` copies the color texture to the CPU and waits for
it. It handles WebGPU's 256-byte row alignment and returns tightly packed rows.
It blocks, so use it in tests and tools, not per frame:

```cpp
auto pixels = target.read_pixels();
REQUIRE(pixels);
auto rgba = pixels->rgba8_at(10, 5);   // BGRA targets are swizzled to RGBA
```

`test/test_gpu_pass.cpp` draws a fullscreen triangle into an offscreen target
and checks known pixels this way; the cases skip on hosts without a native GPU
adapter.

## Completion

`frame.submit()` returns a `GpuSubmission`. `status()` reads the current state
without blocking, `poll()` runs pending callbacks once, and `wait(timeout)`
blocks. A submission that completes while the device is lost reports `failed`.
