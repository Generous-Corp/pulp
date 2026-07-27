// plugin_frame_renderer.cpp — see plugin_frame_renderer.hpp for why this
// module exists and what stays in the platform hosts.

#include <pulp/view/plugin_frame_renderer.hpp>

#include <pulp/canvas/canvas.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/trace.hpp>
#include <pulp/view/repaint_damage.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#ifdef PULP_HAS_SKIA
#include <pulp/canvas/skia_canvas.hpp>
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"
#endif

#include <cmath>

namespace pulp::view {

namespace {

/// Resolve the design-viewport letterbox transform for `geometry`, or report
/// that no viewport is active. One place, so the paint body and the clip
/// mapping cannot disagree about where content lands.
bool design_transform(const FrameGeometry& g, float& sx, float& sy, float& tx,
                      float& ty) {
    return g.has_design_viewport() &&
           WindowHost::compute_design_viewport_transform(
               g.width, g.height, g.design_width, g.design_height, sx, sy, tx,
               ty, g.design_top_align);
}

}  // namespace

void paint_plugin_scene(canvas::Canvas& canvas, View& root,
                        const FrameGeometry& geometry, const Rect* clip) {
    // FU-2: clip the ENTIRE body, background fill included. Everything outside
    // the clip must remain the retained scene's previous pixels; filling the
    // background unclipped would erase them.
    const int clip_save = canvas.save_count();
    if (clip) {
        canvas.save();
        canvas.clip_rect(clip->x, clip->y, clip->width, clip->height);
    }

    // Host clear color, not a themed surface: kEditorHostClear* is the
    // pre-first-frame background every Pulp host seeds (shared with the macOS
    // NSColor/CGColor spellings in window_host_mac_internal.hpp) and it must
    // not shift with the active theme.
    canvas.set_fill_color(pulp::canvas::Color::rgba8(  // token-lint:allow
        kEditorHostClearR, kEditorHostClearG, kEditorHostClearB));
    canvas.fill_rect(0, 0, geometry.width, geometry.height);

    float sx, sy, tx, ty;
    if (design_transform(geometry, sx, sy, tx, ty)) {
        root.set_bounds({0, 0, geometry.design_width, geometry.design_height});
        root.layout_children();
        const int saved = canvas.save_count();
        canvas.save();
        canvas.translate(tx, ty);
        canvas.scale(sx, sy);
        root.paint_all(canvas);
        View::paint_overlays(canvas, &root);
        canvas.restore_to_count(saved);
    } else {
        root.set_bounds({0, 0, geometry.width, geometry.height});
        root.layout_children();
        root.paint_all(canvas);
        View::paint_overlays(canvas, &root);
    }

    if (clip) canvas.restore_to_count(clip_save);
}

bool compute_frame_clip(View& root, const PendingDamage::Snapshot& damage,
                        const FrameGeometry& geometry, Rect& out_clip) {
    if (!damage.is_bounded()) return false;

    // The hazard model runs in ROOT space, where the damage was produced. It
    // escalates to a full repaint if anything that SAMPLES at a distance —
    // backdrop-filter, blur, mask, sampling effect, render transform — reaches
    // the damage, which is what makes a clipped repaint pixel-identical to a
    // full one rather than merely close.
    const auto decision = compute_effective_damage(root, damage.bounds(),
                                                   geometry.scale);
    if (decision.full) return false;

    Rect clip = decision.bounds;

    // Under a design viewport the paint body applies translate(tx,ty) +
    // scale(sx,sy), but the clip is installed in SURFACE space, before that
    // transform. Map the root-space damage through the same letterbox
    // transform so the two agree; without this the clip lands in the wrong
    // place, which is why plug-in editors — which always set a design viewport
    // — could not use partial repaint at all.
    float sx, sy, tx, ty;
    if (design_transform(geometry, sx, sy, tx, ty) && sx > 0.0f && sy > 0.0f) {
        clip = Rect{tx + clip.x * sx, ty + clip.y * sy, clip.width * sx,
                    clip.height * sy};
        // Re-snap OUT to whole surface pixels after scaling: a fractional edge
        // would clip a partially covered pixel.
        const float x0 = std::floor(clip.x);
        const float y0 = std::floor(clip.y);
        clip = Rect{x0, y0, std::ceil(clip.x + clip.width) - x0,
                    std::ceil(clip.y + clip.height) - y0};
    }

    if (clip.width <= 0.0f || clip.height <= 0.0f) return false;
    out_clip = clip;
    return true;
}

#ifdef PULP_HAS_SKIA

PluginFrameRenderer::Frame PluginFrameRenderer::render(
    render::GpuSurface& gpu, render::SkiaSurface& skia, PendingDamage& damage,
    const Request& request) {
    Frame frame;
    if (!request.root) return frame;

    if (request.idle) request.idle();

    // Swapchain acquire, instrumented because it is the one part of the frame
    // that BLOCKS: under a Fifo (vsync) present mode GetCurrentTexture() waits
    // for the next refresh. Without this span a trace shows a frame whose
    // children sum to ~2 ms but whose total is 20-45 ms, with nowhere to
    // attribute the difference.
    bool acquired = false;
    {
        PULP_TRACE_SCOPE_NAMED("gpu", "gpu_acquire");
        acquired = gpu.begin_frame();
    }
    if (!acquired) {
        // Nothing was consumed, so there is nothing to restore. A failed
        // acquire is transient (the swapchain is busy or being resized).
        frame.outcome = render::FrameOutcome::failed;
        return frame;
    }

    // Take the damage only once the frame is actually going to be painted, and
    // in ONE step — reading the accessors and clearing separately is how a host
    // ends up retiring a region it never painted.
    const PendingDamage::Snapshot consumed = damage.take();

    auto* canvas = skia.begin_frame();
    if (!canvas) {
        gpu.end_frame();
        damage.restore(consumed);
        return finish(frame, skia.last_frame_outcome());
    }

    Rect clip_rect{};
    const Rect* clip = nullptr;
    if (request.partial_repaint &&
        compute_frame_clip(*request.root, consumed, request.geometry, clip_rect))
        clip = &clip_rect;

    paint_plugin_scene(*canvas, *request.root, request.geometry, clip);

    if (request.capture) {
        // read_current_rgba finalizes + submits the open frame's recording
        // before readback (see the SkiaSurface contract), so no separate flush.
        std::uint32_t pw = 0, ph = 0;
        frame.readback_ok = skia.read_current_rgba(*request.capture, pw, ph) &&
                            !request.capture->empty() && pw > 0 && ph > 0;
        if (request.capture_width) *request.capture_width = pw;
        if (request.capture_height) *request.capture_height = ph;
    }

    const render::FrameOutcome outcome = skia.end_frame();
    gpu.end_frame();

    if (!render::frame_reached_output(outcome)) {
        // The frame never reached the drawable. Put the damage back so the
        // retry repaints at least as much as this attempt was meant to, and
        // escalate to full: after a failed present the drawable's contents are
        // undefined, so a bounded retry could composite onto garbage.
        damage.restore(consumed);
        damage.mark_full();
    }

    return finish(frame, outcome);
}

PluginFrameRenderer::Frame& PluginFrameRenderer::finish(
    Frame& frame, render::FrameOutcome outcome) {
    frame.outcome = outcome;
    if (outcome == render::FrameOutcome::recreate) {
        ++consecutive_recreates_;
        frame.should_recreate_surface =
            consecutive_recreates_ <= kMaxConsecutiveRecreates;
        frame.gpu_path_exhausted = !frame.should_recreate_surface;
    } else if (render::frame_reached_output(outcome)) {
        consecutive_recreates_ = 0;
    }
    return frame;
}

render::GpuSurface::Config editor_surface_config(
    void* native_handle, const FrameGeometry& geometry,
    PluginViewHost::PresentPolicy policy, bool enable_gpu_timing) {
    render::GpuSurface::Config cfg{};
    // The GPU surface is sized in PHYSICAL pixels so the swapchain matches the
    // HiDPI display; the view tree stays in logical units.
    cfg.width = geometry.pixel_width();
    cfg.height = geometry.pixel_height();
    cfg.native_surface_handle = native_handle;
    cfg.vsync = (policy == PluginViewHost::PresentPolicy::vsync);
    // Diagnostics are opt-in: requesting timestamp queries forces Dawn's
    // allow_unsafe_apis toggle, which weakens validation for ordinary
    // rendering too.
    cfg.enable_gpu_timing = enable_gpu_timing;
    return cfg;
}

EditorSurfaces create_editor_surfaces(void* native_handle,
                                      const FrameGeometry& geometry,
                                      PluginViewHost::PresentPolicy policy,
                                      bool enable_gpu_timing,
                                      bool want_partial_repaint,
                                      const char* log_tag) {
    EditorSurfaces out;
    runtime::log_info("{}: init GPU logical={}x{} physical={}x{} scale={}", log_tag,
                      geometry.width, geometry.height, geometry.pixel_width(),
                      geometry.pixel_height(), geometry.scale);

    out.gpu = render::GpuSurface::create_dawn();
    if (!out.gpu) {
        runtime::log_warn("{}: gpu create_dawn failed; CPU raster only", log_tag);
        return out;
    }

    const render::GpuSurface::Config cfg =
        editor_surface_config(native_handle, geometry, policy,
                              enable_gpu_timing);
    if (!out.gpu->initialize(cfg)) {
        runtime::log_warn("{}: gpu initialize failed; CPU raster only", log_tag);
        out.gpu.reset();
        return out;
    }

    render::SkiaSurface::Config scfg{};
    scfg.width = static_cast<std::uint32_t>(geometry.width);    // LOGICAL
    scfg.height = static_cast<std::uint32_t>(geometry.height);  // LOGICAL
    scfg.scale_factor = geometry.scale;
    out.skia = render::SkiaSurface::create(*out.gpu, scfg);
    if (!out.skia) {
        runtime::log_warn("{}: skia surface create failed; CPU raster only", log_tag);
        out.gpu.reset();
        return out;
    }

    // Negotiate partial repaint only after the surface exists: the swapchain
    // does not preserve content between frames, so a clipped repaint is correct
    // only against a retained scene target the backend agreed to keep.
    out.partial_repaint = want_partial_repaint;
    if (out.partial_repaint && !out.skia->set_persistent_scene(true)) {
        out.partial_repaint = false;
        runtime::log_warn("{}: backend cannot retain a scene; partial repaint "
                          "disabled", log_tag);
    }

    runtime::log_info("{}: GPU and Skia surfaces ready", log_tag);
    return out;
}

std::vector<std::uint8_t> raster_plugin_scene_rgba(View& root,
                                                   const FrameGeometry& geometry,
                                                   std::uint32_t* out_w,
                                                   std::uint32_t* out_h) {
    const std::uint32_t w = geometry.pixel_width();
    const std::uint32_t h = geometry.pixel_height();
    if (w == 0 || h == 0) return {};

    const SkImageInfo info = SkImageInfo::Make(
        static_cast<int>(w), static_cast<int>(h), kRGBA_8888_SkColorType,
        kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto surface = SkSurfaces::Raster(info);
    if (!surface) return {};
    auto* sk_canvas = surface->getCanvas();
    if (!sk_canvas) return {};

    // The DPI scale is a canvas transform, not a geometry change, so the paint
    // below still works in logical units and the result is crisp on HiDPI at
    // the same resolution the GPU surface would have used.
    if (geometry.scale != 1.0f) sk_canvas->scale(geometry.scale, geometry.scale);
    canvas::SkiaCanvas canvas(sk_canvas);
    paint_plugin_scene(canvas, root, geometry, nullptr);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) * h * 4u);
    SkPixmap pixmap(info, pixels.data(), static_cast<std::size_t>(w) * 4u);
    if (!surface->readPixels(pixmap, 0, 0)) return {};
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return pixels;
}

std::vector<std::uint8_t> encode_rgba_png(const std::vector<std::uint8_t>& rgba,
                                          std::uint32_t w, std::uint32_t h) {
    if (rgba.empty() || w == 0 || h == 0) return {};
    const SkImageInfo info = SkImageInfo::Make(
        static_cast<int>(w), static_cast<int>(h), kRGBA_8888_SkColorType,
        kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    SkPixmap pixmap(info, rgba.data(), static_cast<std::size_t>(w) * 4u);
    // The 2-arg pixmap overload returns sk_sp<SkData>; the 3-arg SkWStream*
    // overload returns bool. Picking the wrong one compiles and silently
    // discards the encoded bytes.
    sk_sp<SkData> png = SkPngEncoder::Encode(pixmap, SkPngEncoder::Options{});
    if (!png || png->isEmpty()) return {};
    const auto* p = static_cast<const std::uint8_t*>(png->data());
    return std::vector<std::uint8_t>(p, p + png->size());
}

#endif  // PULP_HAS_SKIA

}  // namespace pulp::view
