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

#if defined(__EMSCRIPTEN__)
    /// Browser (WebGL2) surface configuration.
    ///
    /// `canvas_selector` is a CSS selector for the target `<canvas>` element,
    /// resolved by Emscripten's HTML5 WebGL API.
    struct WebGlConfig {
        const char* canvas_selector = "#pulp";
        uint32_t width = 0;
        uint32_t height = 0;
        float scale_factor = 1.0f;  // devicePixelRatio
    };

    /// Create a Skia surface backed by Ganesh on a WebGL2 context.
    ///
    /// There is no GpuSurface: GpuSurface is a Dawn abstraction and has no
    /// WebGL analogue, so a browser host reports `gpu_surface() == nullptr`
    /// and the JS GPU bridge degrades accordingly. Presentation is implicit —
    /// the browser composites the canvas element, so `end_frame()` submits and
    /// there is nothing to present.
    ///
    /// The returned surface's `graphite_context()` is always `nullptr`: this
    /// backend is Ganesh, not Graphite. Callers that need a Graphite `Context`
    /// (e.g. `SkpFrameCapture` GPU-image readback) must handle the null.
    static std::unique_ptr<SkiaSurface> create_webgl(const WebGlConfig& config);
#endif

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

    /// Enable/disable persistent-scene mode (partial-repaint support, FU-2).
    /// Returns whether the mode is ACTIVE after the call.
    ///
    /// In this mode `begin_frame()` targets a persistent, window-sized Graphite
    /// scene surface that RETAINS its content across frames; `end_frame()` blits
    /// that scene 1:1 onto the presentable drawable, then submits. This is what
    /// makes a clipped (partial) repaint correct on the non-preserving Dawn
    /// swapchain: the host repaints only the damaged rect into the retained
    /// scene and the whole scene is re-presented, so pixels outside the clip are
    /// the previous frame's — exactly the "static chrome doesn't re-composite"
    /// win. The first frame after enabling (or after a resize) is a full repaint
    /// into the fresh scene; the host arms that.
    ///
    /// Default: unsupported no-op returning `false` — the base and the
    /// Ganesh/WebGL2 surface do not implement it, so a `false` return tells the
    /// caller it must full-repaint every frame. Only the Dawn/Graphite desktop
    /// surface implements it.
    virtual bool set_persistent_scene(bool /*enable*/) { return false; }

    /// Whether persistent-scene mode is currently active.
    virtual bool persistent_scene() const { return false; }

    /// Read the current rendered frame into an RGBA buffer. If a frame is
    /// currently open, this finalizes and submits pending canvas work before
    /// readback; callers should treat readback as a capture/end-of-frame
    /// operation and not continue drawing into the same frame afterward.
    virtual bool read_current_rgba(std::vector<uint8_t>& pixels,
                                   uint32_t& pixel_width,
                                   uint32_t& pixel_height) = 0;

    /// Check if Skia rendering is available
    virtual bool is_available() const = 0;

    /// The live Graphite `Context` backing this surface, or `nullptr`
    /// when Skia/Graphite is unavailable — including on the Ganesh/WebGL2
    /// backend, which has no Graphite context at all. Hand this to a `SkpFrameCapture`
    /// (or `capture_skp_to_file`) so a `.skp` of this surface's frame can
    /// read back any GPU-texture-backed embedded images instead of
    /// silently dropping them. The pointer is owned by the SkiaSurface.
    virtual skgpu::graphite::Context* graphite_context() const = 0;

    /// Most recent whole-recording GPU *render* time in milliseconds, sampled
    /// from Skia Graphite's GpuStats(kElapsedTime) finished-with-stats
    /// callback. Returns 0 until the first sample lands or when timing is
    /// unavailable. This is render-recording time, not total frame time (on the
    /// Metal fallback it excludes non-pass uploads/copies and present). See
    /// `gpu_render_time.hpp`.
    virtual double gpu_render_time_ms() const = 0;

    /// Whether GPU render timing is available this run — the adapter offered
    /// `timestamp-query` AND Graphite advertises `kElapsedTime`. When false the
    /// inspector should show the honest "GPU timing unavailable".
    virtual bool gpu_render_timing_available() const = 0;
};

} // namespace pulp::render
