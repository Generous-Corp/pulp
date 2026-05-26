#pragma once

#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulp::canvas { class Canvas; }

namespace pulp::render {

/// One-call wrapper for the existing offscreen Dawn + Skia path
/// (`GpuSurface::create_dawn() + native_surface_handle=nullptr` +
/// `SkiaSurface::create`). Intended for CI golden tests and any other
/// callsite that wants "render this scene to an RGBA / PNG buffer with
/// no window". Spec: planning/2026-05-24-macos-plugin-authoring-plan.md §6.7.
///
/// The wrapper does NOT introduce a new render path — it just hides
/// the begin_frame/end_frame ceremony and the GPU readback dance so a
/// test can do:
///
///   auto h = HeadlessSurface::create({640, 360});
///   if (!h || !h->is_ready()) {
///       SUCCEED("Dawn/Graphite unavailable — golden skipped.");
///       return;
///   }
///   auto rgba = h->render_rgba([&](canvas::Canvas& c) {
///       paint_my_scene(c);
///   });
///   auto png = HeadlessSurface::encode_png(rgba);
///
/// When Skia/Dawn isn't linked (the `PULP_HAS_SKIA=0` build), the
/// factory returns nullptr and `last_error()` carries the reason so
/// the caller can soft-skip. The header is always safe to include —
/// no Skia/Dawn types leak into it.
class HeadlessSurface {
public:
    struct Config {
        uint32_t width = 800;
        uint32_t height = 600;
        /// HiDPI scale factor handed to SkiaSurface::Config.
        float scale_factor = 1.0f;
        /// Background fill applied before the user paint callback runs.
        /// Defaults to opaque black so a no-op paint still produces a
        /// deterministic frame (read-back uninitialized GPU memory is
        /// not reproducible — see plan §6.7 acceptance).
        uint8_t clear_r = 0;
        uint8_t clear_g = 0;
        uint8_t clear_b = 0;
        uint8_t clear_a = 255;
    };

    struct Rgba {
        std::vector<uint8_t> pixels;  ///< RGBA, 4 bytes/pixel, row-major top→bottom
        uint32_t width = 0;
        uint32_t height = 0;

        bool empty() const { return pixels.empty() || width == 0 || height == 0; }
        size_t pixel_count() const { return static_cast<size_t>(width) * height; }
    };

    /// Caller-supplied paint callback. The Canvas is the live Skia
    /// canvas for the current offscreen frame. The wrapper applies the
    /// background fill BEFORE invoking this callback.
    using PaintFn = std::function<void(canvas::Canvas&)>;

    /// Create a headless offscreen surface, or `nullptr` if Skia/Dawn
    /// aren't available or the underlying GpuSurface/SkiaSurface
    /// initialization failed. `last_error_out` (if non-null) is filled
    /// with a human-readable reason in the failure case.
    static std::unique_ptr<HeadlessSurface> create(
        const Config& config,
        std::string* last_error_out = nullptr);

    virtual ~HeadlessSurface() = default;

    /// True when the underlying GpuSurface + SkiaSurface are live and
    /// ready to accept paint callbacks. False after a failed init or
    /// when the device was lost.
    virtual bool is_ready() const = 0;

    /// Run one offscreen frame: begin_frame → fill → user paint → read
    /// pixels → end_frame. Returns an empty `Rgba` on failure (callers
    /// should soft-skip when `is_ready()` was true but readback failed
    /// — the GPU adapter may be too constrained for readback).
    virtual Rgba render_rgba(const PaintFn& paint) = 0;

    /// Convenience: render and PNG-encode in one call. Returns empty
    /// bytes on failure (same semantics as `render_rgba`).
    virtual std::vector<uint8_t> render_png(const PaintFn& paint) = 0;

    /// Last error message recorded by the wrapper. Empty when no error.
    virtual std::string last_error() const = 0;

    /// PNG-encode an already-captured RGBA buffer. Returns empty on
    /// failure. Available even on builds where the runtime surface
    /// itself isn't (e.g. host-side post-processing of a fixture
    /// snapshot). When `PULP_HAS_SKIA` is off the implementation
    /// returns empty bytes and records a reason in `error_out`.
    static std::vector<uint8_t> encode_png(const Rgba& rgba,
                                           std::string* error_out = nullptr);

    /// Cheap deterministic fingerprint of an RGBA buffer — FNV-1a 64.
    /// Useful for golden-bytes assertions where storing the full PNG
    /// in-tree is overkill but the test still wants drift detection.
    /// Independent of Skia, always available.
    static uint64_t rgba_fingerprint(const Rgba& rgba);
};

} // namespace pulp::render
