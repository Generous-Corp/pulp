#pragma once

// plugin_frame_renderer.hpp — the damage-aware frame pipeline shared by the
// Windows and Linux plug-in editor hosts.
//
// Both hosts had their own copy of: the background fill and design-viewport
// paint body, the damage → clip decision (hazard model, letterbox mapping,
// pixel snapping), and the acquire → paint → readback → submit → present
// sequence. The copies had already drifted — only one of them checked the
// readback result — and every WAH-2 failure-semantics fix would otherwise have
// had to be made twice, correctly, forever.
//
// The split here is deliberate:
//
//   * `FrameGeometry`, `compute_frame_clip()` and `paint_plugin_scene()` are
//     pure and Skia-free, so they compile and are TESTED on macOS — which is
//     Pulp's required CI gate. Logic reachable only under `_WIN32` is logic no
//     gate ever runs (the same reasoning as win_pointer_input.hpp).
//   * `PluginFrameRenderer` owns the GPU drive and the failure policy, and only
//     exists in Skia builds.
//
// Native window ownership (HWND/X11), event dispatch, and attach/detach stay in
// the platform hosts. This module never touches a native handle.

#include <pulp/view/geometry.hpp>
#include <pulp/view/pending_damage.hpp>

#include <cstdint>

namespace pulp::canvas {
class Canvas;
}

namespace pulp::view {

class View;

// ── Editor host clear / background color ─────────────────────────────────
//
// Every Pulp host — the standalone window hosts and all four plug-in editor
// hosts — seeds the same opaque dark background (RGB 30,30,46 = 0x1E1E2E) so
// no clear/undefined composite flashes before the first GPU frame lands, and
// so the letterbox bars around a design viewport match the editor rather than
// showing the DAW's own window content.
//
// It lives HERE, platform-neutrally, because it is a property of the frame
// pipeline, not of any one OS. macOS additionally needs NSColor and CGColor
// spellings of the same value (window_host_mac_internal.hpp), which is exactly
// the kind of divergence a single definition prevents.
inline constexpr std::uint8_t kEditorHostClearR = 30;
inline constexpr std::uint8_t kEditorHostClearG = 30;
inline constexpr std::uint8_t kEditorHostClearB = 46;

/// The size/scale/viewport a frame is painted against, in one value.
///
/// `width`/`height` are LOGICAL (the view tree's units). `scale` maps them to
/// physical pixels. A non-zero `design_*` pair pins the root to that size and
/// letterboxes it into width×height — the mode every plug-in editor runs in.
struct FrameGeometry {
    float width = 0.0f;
    float height = 0.0f;
    float scale = 1.0f;
    float design_width = 0.0f;   ///< 0 disables the design viewport
    float design_height = 0.0f;
    bool design_top_align = false;

    bool has_design_viewport() const {
        return design_width > 0.0f && design_height > 0.0f;
    }

    /// Physical-pixel dimensions = logical × scale, clamped to at least 1 so a
    /// collapsed editor cannot ask for a zero-sized surface.
    std::uint32_t pixel_width() const { return to_pixels(width); }
    std::uint32_t pixel_height() const { return to_pixels(height); }

private:
    std::uint32_t to_pixels(float logical) const {
        const float p = logical * scale;
        return static_cast<std::uint32_t>(p < 1.0f ? 1.0f : p);
    }
};

/// Decide the clip rect for a partial repaint, in SURFACE space.
///
/// Returns false when the frame must repaint in full — because the damage is
/// full, because there is no bounded damage, or because the hazard model
/// (`compute_effective_damage`) found something that samples at a distance
/// reaching the damaged area, which is what makes a clipped repaint
/// pixel-identical to a full one rather than merely similar.
///
/// On success `out_clip` is already mapped through the design-viewport
/// letterbox transform and snapped OUT to whole surface pixels: the clip is
/// installed BEFORE paint applies translate+scale, so an unmapped root-space
/// rect would land in the wrong place, and a fractional edge would clip a
/// partially covered pixel.
bool compute_frame_clip(View& root, const PendingDamage::Snapshot& damage,
                        const FrameGeometry& geometry, Rect& out_clip);

/// Paint one editor frame: background fill, design-viewport transform, view
/// tree, overlays.
///
/// When `clip` is non-null the ENTIRE body is clipped, background fill
/// included — everything outside the clip must remain the retained scene's
/// previous pixels, and an unclipped background fill would erase them.
void paint_plugin_scene(canvas::Canvas& canvas, View& root,
                        const FrameGeometry& geometry, const Rect* clip);

}  // namespace pulp::view

#ifdef PULP_HAS_SKIA

#include <pulp/render/gpu_surface.hpp>
#include <pulp/render/skia_surface.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace pulp::render {
class GpuSurface;
}

namespace pulp::view {

/// Drives one GPU frame for a plug-in editor host, and owns what happens when
/// the frame does not reach the screen.
///
/// The failure policy is the whole point, and it is shared so Windows and Linux
/// cannot diverge on it:
///
///   * damage is CONSUMED for the frame and RESTORED when the frame did not
///     reach its output, so a failed frame's damage survives into the retry
///     instead of being cleared by a `success` that never happened;
///   * a `recreate` outcome asks the host to rebuild its surfaces, bounded by
///     `kMaxConsecutiveRecreates` so a permanently broken drawable degrades to
///     the host's CPU raster path rather than spinning on surface creation;
///   * a readback failure is reported separately from a present failure — a
///     capture can fail on a frame that reached the screen perfectly.
class PluginFrameRenderer {
public:
    /// How many consecutive recreate requests to honour before declaring the
    /// GPU path unusable. Three covers a transient device-lost/resize race;
    /// beyond that the drawable is not coming back and a black editor forever
    /// is worse than the CPU fallback.
    static constexpr int kMaxConsecutiveRecreates = 3;

    struct Frame {
        render::FrameOutcome outcome = render::FrameOutcome::failed;
        /// False only when a capture was requested and did not produce pixels.
        bool readback_ok = true;
        /// The host should tear down and rebuild its GPU/Skia surfaces before
        /// the next frame. Never true once the recreate budget is exhausted.
        bool should_recreate_surface = false;
        /// The GPU path has failed too many times in a row; the host should
        /// stop using it and fall back to CPU raster.
        bool gpu_path_exhausted = false;

        bool reached_output() const {
            return render::frame_reached_output(outcome);
        }
    };

    struct Request {
        View* root = nullptr;
        FrameGeometry geometry{};
        bool partial_repaint = false;
        /// Per-frame idle pump (scripted UI timers / rAF). Called once, before
        /// the swapchain is acquired.
        std::function<void()> idle;
        /// Optional RGBA capture of this frame. Null means "no capture".
        std::vector<std::uint8_t>* capture = nullptr;
        std::uint32_t* capture_width = nullptr;
        std::uint32_t* capture_height = nullptr;
    };

    /// Render one frame. `damage` is taken at the start and restored if the
    /// frame did not reach its output.
    Frame render(render::GpuSurface& gpu, render::SkiaSurface& skia,
                 PendingDamage& damage, const Request& request);

    /// Reset the recreate budget. Hosts call this after successfully building a
    /// fresh surface pair, so an editor that recovers is not penalised for
    /// earlier failures.
    void note_surfaces_created() { consecutive_recreates_ = 0; }

    int consecutive_recreates() const { return consecutive_recreates_; }

private:
    /// Apply the recreate/exhaustion policy for `outcome` and stamp it onto
    /// `frame`. Every exit path from render() goes through here, so a new early
    /// return cannot accidentally skip the budget accounting.
    Frame& finish(Frame& frame, render::FrameOutcome outcome);

    int consecutive_recreates_ = 0;
};

/// The surface pair an editor renders through, plus the partial-repaint
/// decision that can only be made once the backend has answered.
struct EditorSurfaces {
    std::unique_ptr<render::GpuSurface> gpu;
    std::unique_ptr<render::SkiaSurface> skia;
    /// True only if partial repaint was requested AND the backend agreed to
    /// retain a scene target. A clipped repaint against a non-preserving
    /// swapchain shows stale garbage outside the clip, so this must never be
    /// left on speculatively.
    bool partial_repaint = false;

    bool ok() const { return gpu != nullptr && skia != nullptr; }
};

/// Build the GPU + Skia surface pair for a plug-in editor window.
///
/// Shared by the Windows and Linux hosts, which had grown near-identical copies
/// that had already DIVERGED in a way nobody could see by reading either file
/// alone: only the Windows copy set `vsync = false`. The default Fifo present
/// mode makes the next `GetCurrentTexture()` block until the display's refresh,
/// which is right for a standalone app that owns its frame loop and wrong for a
/// plug-in editor rendering synchronously on the DAW's UI thread — it stalls
/// the message pump that delivers the very input being dragged. Measured on the
/// REAPER VM with Perfetto: ~2 ms of actual frame work inside 19–45 ms frames,
/// the rest all acquire, and 7 frames produced across 8 drag sweeps.
///
/// Making it one function makes that policy true for both hosts by
/// construction rather than by two people remembering.
///
/// Does NOT publish surface state: `publish_gpu_surface()` is the host's, and
/// each host has its own rule for what a failure means to an attach in flight.
///
/// `native_handle` is the platform's surface handle — an `HWND` on Windows, a
/// pointer to the typed X11 handle on Linux. `log_tag` names the host in
/// diagnostics.
EditorSurfaces create_editor_surfaces(void* native_handle,
                                      const FrameGeometry& geometry,
                                      bool want_partial_repaint,
                                      const char* log_tag);

/// CPU-raster one editor frame at physical-pixel resolution, returning tightly
/// packed RGBA8888 and reporting the pixel dimensions through `out_w`/`out_h`.
/// Empty on any failure.
///
/// Shared because Windows and Linux carried byte-identical copies of it: the
/// same surface, the same `scale` canvas transform so `paint_plugin_scene()`
/// keeps working in logical units, the same read-back. It is the only render
/// path guaranteed to exist — the headless capture that proves a non-black
/// frame with no GPU at all — so a divergence between the two copies would
/// have meant the proof differed from the thing it proves.
std::vector<std::uint8_t> raster_plugin_scene_rgba(View& root,
                                                   const FrameGeometry& geometry,
                                                   std::uint32_t* out_w,
                                                   std::uint32_t* out_h);

/// Encode tightly packed RGBA8888 pixels as a PNG byte stream. Empty on
/// failure or on empty input.
std::vector<std::uint8_t> encode_rgba_png(const std::vector<std::uint8_t>& rgba,
                                          std::uint32_t w, std::uint32_t h);

}  // namespace pulp::view

#endif  // PULP_HAS_SKIA
