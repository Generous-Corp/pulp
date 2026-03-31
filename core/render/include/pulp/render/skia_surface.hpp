#pragma once

#include <pulp/render/gpu_surface.hpp>
#include <pulp/canvas/canvas.hpp>
#include <memory>
#include <functional>
#include <vector>

namespace pulp::render {

/// Skia Graphite rendering surface.
///
/// Connects a GpuSurface (WebGPU device) to Skia's GPU 2D rendering.
/// When GpuSurface has a presentable surface, SkiaSurface renders into the
/// swapchain texture each frame (zero-copy presentation). When offscreen,
/// it renders into a standalone GPU texture.
///
/// Usage:
///   gpu->begin_frame();                    // acquire swapchain texture
///   auto* canvas = skia->begin_frame();    // get Skia canvas targeting that texture
///   root_view.paint_all(*canvas);
///   skia->end_frame();                     // submit Graphite recording
///   gpu->end_frame();                      // present to native surface
class SkiaSurface {
public:
    struct Config {
        uint32_t width = 800;
        uint32_t height = 600;
        float scale_factor = 1.0f;  // HiDPI
    };

    /// Create a Skia Graphite surface.
    /// The GpuSurface reference is retained — SkiaSurface queries it each frame
    /// for the current swapchain texture when on-screen presentation is active.
    static std::unique_ptr<SkiaSurface> create(GpuSurface& gpu, const Config& config);

    virtual ~SkiaSurface() = default;

    /// Begin a frame: returns a Canvas to draw into.
    /// If GpuSurface has a presentable surface, the canvas targets the current
    /// swapchain texture. Otherwise, it targets an offscreen render target.
    /// The canvas is valid until end_frame() is called.
    virtual canvas::Canvas* begin_frame() = 0;

    /// End a frame: submits the Graphite recording to the GPU.
    /// Actual presentation is handled by GpuSurface::end_frame().
    virtual void end_frame() = 0;

    /// Resize the surface
    virtual void resize(uint32_t width, uint32_t height, float scale = 1.0f) = 0;

    /// Read the current rendered frame into an RGBA buffer.
    virtual bool read_current_rgba(std::vector<uint8_t>& pixels,
                                   uint32_t& pixel_width,
                                   uint32_t& pixel_height) = 0;

    /// Check if Skia rendering is available
    virtual bool is_available() const = 0;
};

} // namespace pulp::render
