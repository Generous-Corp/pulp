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

#include <pulp/render/skia_surface.hpp>

#include <cstdint>
#include <functional>
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

}  // namespace pulp::view

#endif  // PULP_HAS_SKIA
