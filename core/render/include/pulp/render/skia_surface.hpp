#pragma once

#include <pulp/render/gpu_surface.hpp>
#include <pulp/canvas/canvas.hpp>
#include <memory>
#include <functional>
#include <vector>

namespace skgpu::graphite {
class Context;
}

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

    /// The live Graphite `Context` backing this surface, or `nullptr`
    /// when Skia/Graphite is unavailable. Hand this to a `SkpFrameCapture`
    /// (or `capture_skp_to_file`) so a `.skp` of this surface's frame can
    /// read back any GPU-texture-backed embedded images instead of
    /// silently dropping them. The pointer is owned by the SkiaSurface.
    virtual skgpu::graphite::Context* graphite_context() const = 0;

    /// Phase 6.5 (re-scoped) — most recent whole-recording GPU *render* time
    /// in milliseconds, sampled from Skia Graphite's GpuStats(kElapsedTime)
    /// finished-with-stats callback. Returns 0 until the first sample lands or
    /// when timing is unavailable. This is render-recording time, not total
    /// frame time (on the Metal fallback it excludes non-pass uploads/copies
    /// and present). See `gpu_render_time.hpp` and
    /// `planning/2026-05-21-gpu-timestamp-readback-proposal.md`.
    virtual double gpu_render_time_ms() const = 0;

    /// Whether GPU render timing is available this run — the adapter offered
    /// `timestamp-query` AND Graphite advertises `kElapsedTime`. When false the
    /// inspector should show the honest "GPU timing unavailable".
    virtual bool gpu_render_timing_available() const = 0;
};

} // namespace pulp::render
