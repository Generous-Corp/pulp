#pragma once

#include <pulp/render/gpu_surface.hpp>
#include <pulp/canvas/canvas.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <functional>
#include <vector>

namespace skgpu::graphite {
class Context;
}

namespace pulp::render {

/// Where a frame actually ended up.
///
/// A frame that renders perfectly and reaches nothing is the failure mode this
/// enum exists to stop being silent. Before it, `begin_frame()` answered a
/// failed native-drawable wrap by handing back the OFFSCREEN canvas: the host
/// painted, submitted, presented, reported success, and cleared its damage,
/// while the texture being presented had never been drawn into. The editor was
/// black and every observable signal — including screenshot readback, which
/// reads the offscreen target — said the frame was fine.
///
/// `offscreen` is therefore a first-class SUCCESS (headless capture and the
/// browser path have no presentable surface by design), and the offscreen
/// canvas is never a fallback for a presentable surface that failed.
enum class FrameOutcome {
    /// Drew into, and submitted to, the intended presentable drawable.
    presented,
    /// No native surface exists by design; the offscreen target IS the output.
    offscreen,
    /// The native drawable could not be wrapped or blitted. Its contents are
    /// undefined; the surface should be recreated before the next attempt.
    recreate,
    /// Submission failed. The surface is still usable — retry the frame.
    failed,
};

/// True when `outcome` means the frame's pixels reached their intended output,
/// i.e. the caller may retire this frame's damage. The one predicate hosts
/// should use, so "did it reach the screen?" cannot drift between platforms.
constexpr bool frame_reached_output(FrameOutcome outcome) {
    return outcome == FrameOutcome::presented ||
           outcome == FrameOutcome::offscreen;
}

/// GPU bytes held by a surface's Skia resource caches.
///
/// `budgeted` counts every resource charged against the cache budget, whether
/// or not something still uses it. `purgeable` is the subset nothing uses any
/// more: textures an image cache released, scratch targets from earlier frames.
/// Those stay resident until a trim or budget pressure frees them, so
/// `purgeable` is what a trim can return to the device.
struct GpuCacheBytes {
    std::size_t budgeted = 0;
    std::size_t purgeable = 0;
};

/// When a surface's idle trim runs. `end_frame()` asks `due()` once per
/// frame, so the steady-state cost is one timestamp compare; a trim runs at
/// most once per `kInterval` and frees only resources unused for `kUnusedAge`.
class GpuIdleTrimSchedule {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kInterval{1000};
    static constexpr std::chrono::milliseconds kUnusedAge{2000};

    explicit GpuIdleTrimSchedule(Clock::time_point start) : last_(start) {}

    /// True when a trim is due at `now`; records `now` as the last trim.
    bool due(Clock::time_point now) {
        if (now - last_ < kInterval)
            return false;
        last_ = now;
        ++trims_;
        return true;
    }

    /// Trims `due()` has scheduled since construction.
    std::uint64_t trims() const {
        return trims_;
    }

  private:
    Clock::time_point last_;
    std::uint64_t trims_ = 0;
};

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

    /// Begin a frame: returns a Canvas to draw into, or nullptr on failure.
    ///
    /// The canvas targets, in order: the retained scene surface in
    /// persistent-scene mode; the current swapchain texture when GpuSurface has
    /// a presentable surface; the offscreen render target when it does not.
    /// The canvas is valid until end_frame() is called.
    ///
    /// Returns nullptr — rather than quietly redirecting to the offscreen
    /// target — when a PRESENTABLE surface exists but its drawable could not be
    /// wrapped. The caller must not treat that as a rendered frame; query
    /// last_frame_outcome() for whether to recreate the surface or just retry.
    virtual canvas::Canvas* begin_frame() = 0;

    /// End a frame: submits the Graphite recording to the GPU, and reports
    /// whether the frame reached its intended output.
    ///
    /// Actual presentation is handled by GpuSurface::end_frame(); this covers
    /// everything up to and including submission of the drawable's contents, so
    /// a `presented` result means "the correct texture was drawn into and
    /// submitted", not "the compositor has shown it".
    ///
    /// Hosts must not retire pending damage unless frame_reached_output() is
    /// true for the returned outcome, or the failed frame's damage is lost and
    /// the retry paints nothing.
    virtual FrameOutcome end_frame() = 0;

    /// Outcome of the most recent begin_frame()/end_frame() pair. Set by
    /// begin_frame() too, so a caller that got a null canvas can distinguish
    /// "recreate the surface" from "retry" without a second entry point.
    virtual FrameOutcome last_frame_outcome() const { return FrameOutcome::offscreen; }

    /// Whether this surface has a presentable native drawable. False for
    /// headless/offscreen surfaces, where `offscreen` is the intended output
    /// rather than a fallback.
    virtual bool has_presentable_target() const = 0;

    /// Record a new logical size and backing scale. The render targets are
    /// reallocated, and on a scale change the retained layers dropped, only in
    /// the next begin_frame(), so several resizes before one frame cost one
    /// reallocation.
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

    /// Frames submitted to the GPU whose work has not finished yet (see
    /// `GpuFramesInFlight` in `gpu_render_time.hpp`), or -1 when the surface
    /// does not track it. Diagnostic only; nothing waits on it.
    virtual int gpu_frames_in_flight() const { return -1; }

    /// Block until the GPU has finished the work this surface submitted. A
    /// host that must hand the compositor a finished drawable (a first frame
    /// presented with its Core Animation transaction) calls it between
    /// `end_frame()` and the present. Returns the synchronizing submit's
    /// result: false when the surface has no GPU context or the backend could
    /// not submit and wait. Never call it per frame.
    virtual bool wait_for_submitted_work() {
        return false;
    }

    /// How much a GPU trim frees.
    class TrimLevel {
      public:
        /// Free purgeable resources nothing has used for `idle`. Resources
        /// the next frame is likely to reuse survive.
        static TrimLevel unused_for(std::chrono::milliseconds idle) {
            return TrimLevel(idle, false);
        }
        /// Free every purgeable resource, for a hidden or idle editor.
        static TrimLevel everything() {
            return TrimLevel({}, true);
        }

        bool frees_everything() const {
            return everything_;
        }
        std::chrono::milliseconds idle() const {
            return idle_;
        }

      private:
        TrimLevel(std::chrono::milliseconds idle, bool everything)
            : idle_(idle), everything_(everything) {}
        std::chrono::milliseconds idle_;
        bool everything_;
    };

    /// Return purgeable GPU memory to the device. Call it between frames,
    /// on the render thread: it uses the surface's GPU context, which is not
    /// thread-safe. The same holds for `gpu_cache_bytes()`.
    /// Resources still referenced (cached images, retained layers, the
    /// scene) and work the GPU has not finished are not freed; a host that
    /// wants everything back calls `wait_for_submitted_work()` first.
    ///
    /// `end_frame()` already runs `unused_for(GpuIdleTrimSchedule::kUnusedAge)`
    /// on the `GpuIdleTrimSchedule` throttle, so a host calls this directly
    /// only for `everything()`, when its editor is hidden or stops drawing.
    /// Default: no-op, for surfaces with no GPU cache.
    virtual void trim_gpu_resources(TrimLevel /*level*/) {}

    /// Bytes held by this surface's GPU resource caches. Zero when the
    /// surface has no GPU context. Render thread only.
    virtual GpuCacheBytes gpu_cache_bytes() const {
        return {};
    }

    /// Idle trims `end_frame()` has run since the surface was created.
    virtual std::uint64_t idle_trim_count() const {
        return 0;
    }

    /// Graphite render targets this surface has allocated (the offscreen and
    /// persistent-scene targets; the presentable texture is wrapped, not
    /// allocated). Diagnostic and test use; 0 where it is not tracked.
    virtual std::uint64_t render_target_allocations() const {
        return 0;
    }

#if !defined(__EMSCRIPTEN__)
    /// Test seam for Graphite recording inserts. When installed, every
    /// SkiaSurface passes whether Graphite accepted each recording it inserts
    /// (`end_frame()`'s and `read_current_rgba()`'s mid-frame flush) through
    /// `hook`, and acts on the returned value instead. A healthy device does
    /// not reject recordings, so this is how a test reaches the rejection
    /// path. Process-wide and not synchronized: install before rendering,
    /// pass nullptr to remove.
    using InsertOutcomeHook = bool (*)(bool accepted);
    static void set_insert_outcome_hook_for_testing(InsertOutcomeHook hook);

    /// Test seam for `read_current_rgba()`. Passing false makes every
    /// SkiaSurface skip the asynchronous GPU readback and take the synchronous
    /// fallback that a timed-out readback takes, which a healthy device never
    /// reaches. Process-wide and not synchronized; pass true to restore.
    static void set_async_readback_for_testing(bool enabled);

    /// Test seam for the idle-trim clock. When installed, surfaces read
    /// `hook()` instead of `steady_clock::now()` for the `GpuIdleTrimSchedule`
    /// they own, so a test can step frames through time. It does not reach
    /// Skia's own resource timestamps, which `unused_for` compares against.
    /// Process-wide and not synchronized: install before creating the
    /// surface, pass nullptr to remove.
    using TrimClockHook = GpuIdleTrimSchedule::Clock::time_point (*)();
    static void set_trim_clock_for_testing(TrimClockHook hook);
#endif
};

} // namespace pulp::render
