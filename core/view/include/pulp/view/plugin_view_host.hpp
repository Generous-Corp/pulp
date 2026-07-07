#pragma once

#include <pulp/view/view.hpp>
#include <memory>
#include <cstdint>
#include <functional>
#include <vector>

namespace pulp::render {
class GpuSurface;
}

namespace pulp::view {

// Platform-specific handle types
#ifdef __APPLE__
using NativeViewHandle = void*; // NSView* (macOS) or UIView* (iOS)
#elif defined(_WIN32)
using NativeViewHandle = void*; // HWND
#else
using NativeViewHandle = void*; // X11 Window or nullptr
#endif

// Hosts a View tree inside a DAW plugin window
// Creates a platform-native child view that paints the widget hierarchy
class PluginViewHost {
public:
    struct Size {
        uint32_t width = 400;
        uint32_t height = 300;
    };

    struct Options {
        Size size = {400, 300};
        bool use_gpu = false;  ///< Use GPU rendering (Dawn/Skia Graphite) instead of CoreGraphics
    };

    // Create a plugin view host for the given view tree.
    //
    // Platform support:
    //   - macOS: NSView-backed impl in plugin_view_host_mac.mm, including
    //     native child-view attach/bounds/detach.
    //   - iOS: UIView-backed impl in plugin_view_host_ios.mm, including
    //     native child-view attach/bounds/detach.
    //   - Windows/Linux: native child-window hosts are registered on demand.
    //   - Android or custom hosts: host app registers a factory via
    //     set_factory(). Without a factory, create() returns nullptr
    //     explicitly.
    static std::unique_ptr<PluginViewHost> create(View& root, Size size);
    static std::unique_ptr<PluginViewHost> create(View& root, const Options& options);

    // Host-registered factory. Installed by built-in platform hosts or by
    // embedding apps on targets without a built-in host.
    using Factory = std::function<std::unique_ptr<PluginViewHost>(
        View& root, const Options& options)>;
    static void set_factory(Factory factory);
    static void clear_factory();
    static bool has_factory();

    virtual ~PluginViewHost() = default;

    // Get the native view handle to pass to the DAW host. A non-null host view
    // does not by itself guarantee that this host can also embed other native
    // child views; callers must branch on attach_native_child_view().
    virtual NativeViewHandle native_handle() = 0;

    // Begin a native OUTBOUND file drag from this host's window (drag audio out
    // to Finder/Explorer, a Canvas, another app). Default: unsupported → false.
    //
    // This is the host-owned drag path, distinct from the free function
    // `begin_file_drag(native_view, request)` in drag_drop.hpp. macOS leaves
    // this at the default and uses the free NSDraggingSession backend via
    // native_handle(); Windows (OLE IDataObject/DoDragDrop) and Linux (XDND)
    // override it because their drag needs host-owned native state — the HWND,
    // or the Display* connection plus the interned Xdnd atoms — that the free
    // function does not receive. `View::start_file_drag()` tries this first and
    // only falls back to the free backend when it returns false. Call
    // synchronously from within the pointer interaction that initiates the drag.
    virtual bool start_file_drag(const FileDragRequest& request) {
        (void)request;
        return false;
    }

    // Attach this view to a parent native view (the DAW's editor window)
    virtual void attach_to_parent(NativeViewHandle parent) = 0;

    // Attempt to attach to `parent` and report whether this host ended up
    // attached. The default composes the existing `attach_to_parent()` command
    // with the `is_attached()` query, so every host gets a success signal for
    // free; a host can override to report a more precise outcome. Foreign-host
    // embedders (the flat-C embedding SDK) use this to decide whether to fire
    // `ViewBridge::notify_attached()` — they must NOT fire it when attach failed,
    // or the editor open/close lifecycle goes unbalanced. Call on the UI thread.
    [[nodiscard]] virtual bool try_attach_to_parent(NativeViewHandle parent) {
        attach_to_parent(parent);
        return is_attached();
    }

    // True when this host's native view is currently parented (i.e. attach
    // succeeded and detach has not run). Default is conservative: a host that
    // has not opted into attachment observability reports false, so callers
    // never treat an unknown host as attached. Apple hosts override this to
    // reflect the real native view-hierarchy state. Query on the UI thread.
    virtual bool is_attached() const noexcept { return false; }

    // Detach from parent
    virtual void detach() = 0;

    // Request a repaint (call when parameters change)
    virtual void repaint() = 0;

    // Resize
    virtual void set_size(uint32_t width, uint32_t height) = 0;
    virtual Size get_size() const = 0;

    // True only when this host is rendering through the GPU (Dawn/Skia
    // Graphite) pipeline AND GPU initialization succeeded. CoreGraphics
    // CPU hosts return false, and a GPU host that failed to initialize
    // Dawn/Skia and fell back to CPU also returns false. Adapters read
    // this after create() to scream when a GPU/scripted editor view
    // silently took the CPU path (see `format/gpu_host_select.hpp`).
    virtual bool is_gpu_backed() const { return false; }

    // Mirrors WindowHost::gpu_surface() (window_host.hpp:142). Returns the
    // host's live wgpu::Surface backing, or nullptr when the host is CPU,
    // when GPU init failed, or when no surface has been allocated yet.
    //
    // This is the input port for the JS-side `navigator.gpu` /
    // `canvas.getContext('webgpu')` bridge: WidgetBridge captures this
    // pointer at construction (or via a later attach call) and forwards it
    // to __describeNativeAdapterImpl / __createNativeAdapterImpl /
    // __gpuCanvasConfigureImpl. Without a real surface here, those native
    // impls return mocks and JS-rendered 3D output is black. Always pass
    // the GPU surface to WidgetBridge when constructing a scripted GPU view.
    //
    // Lifetime: the GpuSurface is owned by the PluginViewHost. Consumers
    // capture the raw pointer; the WidgetBridge must be destroyed before
    // the host.
    virtual render::GpuSurface* gpu_surface() const { return nullptr; }

    // Native-frame resize notification. AU v2 has no host size callback — the
    // DAW resizes the returned NSView directly. Hosts invoke this callback
    // when their native view's frame changes (after they have already resized
    // their own surfaces) so the adapter can forward to `ViewBridge::resize`
    // (fires Processor::on_view_resized). VST3/CLAP drive resize through their
    // own host size callbacks and don't need this. Default no-op.
    virtual void set_resize_callback(std::function<void(uint32_t, uint32_t)> cb) {
        (void) cb;
    }

    // Per-vsync idle pump. GPU hosts invoke this callback once per display
    // link tick (before rendering) so a scripted UI's `ScriptedUiSession::poll()`
    // — async results, timers, requestAnimationFrame — keeps running while
    // the editor is embedded. CPU hosts ignore it. Default no-op.
    virtual void set_idle_callback(std::function<void()> callback) {
        (void) callback;
    }

    // Capture the host's current back buffer as a PNG (mirrors
    // `WindowHost::capture_back_buffer_png`, issue #2001). GPU hosts read
    // back the rendered Skia frame via `SkiaSurface::read_current_rgba`;
    // hosts that cannot capture return an empty vector. Used by the
    // embedded-editor host smoke tests.
    virtual std::vector<uint8_t> capture_back_buffer_png() { return {}; }

    // Attach/detach a native child view inside the plugin editor host.
    // Coordinates use Pulp's top-left origin convention, matching WindowHost.
    // Returning false is the canonical unsupported/rejected signal. Non-Apple
    // factory-backed hosts that support native child embedding must override
    // all three methods and should keep attachment state in the concrete host.
    virtual bool attach_native_child_view(NativeViewHandle child_view,
                                          float x,
                                          float y,
                                          float width,
                                          float height) {
        (void) child_view;
        (void) x;
        (void) y;
        (void) width;
        (void) height;
        return false;
    }
    virtual bool set_native_child_view_bounds(NativeViewHandle child_view,
                                              float x,
                                              float y,
                                              float width,
                                              float height) {
        (void) child_view;
        (void) x;
        (void) y;
        (void) width;
        (void) height;
        return false;
    }
    virtual void detach_native_child_view(NativeViewHandle child_view) {
        (void) child_view;
    }

    // Clip an attached native child to `(x, y, width, height)` — the visible
    // sub-rectangle expressed in the child's OWN [0,0,frame_w,frame_h] box, in
    // Pulp's top-left origin convention (same units as
    // set_native_child_view_bounds). The host masks the native view to that rect
    // (e.g. a CALayer mask on Apple) so a child inside a scroll region does not
    // spill past the viewport; masking preserves the child's content size rather
    // than reflowing it by resizing the frame. When has_clip is false, any
    // existing clip is removed. Returns false when unsupported or when the child
    // is not attached to this host. Default: no-op unsupported.
    virtual bool set_native_child_view_clip(NativeViewHandle child_view,
                                            bool has_clip,
                                            float x,
                                            float y,
                                            float width,
                                            float height) {
        (void) child_view;
        (void) has_clip;
        (void) x;
        (void) y;
        (void) width;
        (void) height;
        return false;
    }

    // Pump any pending native drag-and-drop (and related) events on a host
    // that owns its own platform connection. The Linux X11 host overrides this
    // to drain its X connection and run the XDND target protocol (XdndEnter /
    // Position / Drop), routing drops into the view tree via the shared
    // dispatch core. Hosts driven by a host-owned event loop (Apple, the SDL
    // standalone) do not need it. Default no-op. Call on the UI thread; a
    // driver (standalone host idle, or a DAW editor idle hook) invokes it
    // periodically so drags onto the embedded editor are serviced.
    virtual void pump_x_events() {}

    // ── Design viewport (mirrors WindowHost::set_design_viewport) ────────
    //
    // Constrain editor resize to a fixed aspect ratio. Plugin hosts do not
    // own the OS window — the DAW does — so the host does not enforce the
    // aspect itself. Per-format resize-hint paths drive enforcement (CLAP
    // `gui_get_resize_hints.preserve_aspect_ratio`, VST3
    // `checkSizeConstraint`). Stored here purely for API parity with
    // `WindowHost` and so adapters can query it. Default no-op.
    virtual void set_fixed_aspect_ratio(float ratio) { (void)ratio; }

    // Set a fixed "design viewport". When set, the root view's bounds are
    // pinned to (design_w x design_h) and paint applies an aspect-correct
    // scale + letterbox translate to fit the current host size. Mouse
    // coordinates receive the inverse transform before hit-test. Pass
    // (0, 0) to disable.
    //
    // This mirrors `WindowHost::set_design_viewport` (see that method's
    // docs for the design rationale, pulp #59/#63/#64/#65) so that an
    // editor that already paints correctly in the standalone window host
    // paints identically when embedded by a DAW. Default no-op for hosts
    // that do not implement design-viewport scaling.
    virtual void set_design_viewport(float design_w, float design_h) {
        (void)design_w;
        (void)design_h;
    }

    // Anchor the design viewport to the TOP of the host pane instead of
    // centering it vertically (horizontal centering unchanged). The AU v3
    // editor opts in so a fixed-aspect design in a taller host pane (REAPER's
    // FX-chain pane, where AU can't negotiate the pane aspect like CLAP/VST3)
    // reads like CLAP/VST3 — content at the top with the slack as a single
    // bottom strip — instead of floating centered between two bands. Default
    // no-op / centered for every other host + format.
    virtual void set_design_viewport_top_align(bool top_align) {
        (void)top_align;
    }

    // Inverse-map a host-space point (e.g. mouse coords in host logical
    // pixels) into root-view space using the active design-viewport
    // transform. Identity when no design viewport is set. Exposed so
    // native event handlers AND unit tests can exercise the same
    // inverse-transform path without needing AppKit / UIKit event
    // delivery.
    //
    // The input is in host *logical* coordinates (DPI-independent points),
    // matching the view tree's coordinate space. Platforms whose OS delivers
    // input in physical pixels (Windows/Linux) divide by scale_factor() before
    // calling this, so the design-viewport math stays purely logical and
    // composes with HiDPI scale without double-counting it.
    virtual Point window_to_root_point(Point pt) const { return pt; }

    // ── HiDPI scale (W8 Windows / L9 Linux) ─────────────────────────────
    //
    // The DPI scale that maps host *logical* coordinates to physical pixels:
    // the editor's view tree is laid out in logical units (a 100pt knob), the
    // GPU/Skia surface is sized at logical × scale physical pixels, and the
    // SkiaSurface applies this factor as a canvas transform at paint so the UI
    // renders crisply on HiDPI displays instead of at 1× (small/blurry).
    //
    // Platform hosts auto-detect the scale from the OS (Windows:
    // GetDpiForWindow; Linux: Xft.dpi / RANDR) and react to live DPI changes
    // (WM_DPICHANGED). set_scale_factor() lets a DAW/host override the detected
    // value (e.g. a per-editor zoom, or when the host owns DPI policy) and
    // makes the scale plumbing testable without a real display. Passing a
    // non-positive value is ignored. Default impl is a no-op getter returning
    // 1.0 (CPU/stub hosts that do not scale).
    virtual void set_scale_factor(float scale) { (void)scale; }
    virtual float scale_factor() const { return 1.0f; }
};

// Install the built-in platform PluginViewHost factory (and matching headless
// screenshot provider) on non-Apple platforms (#3329 Win/Linux parity).
//
// Idempotent and thread-safe: the first call registers the platform factory via
// PluginViewHost::set_factory(); subsequent calls are no-ops. A host that wants
// a custom factory can call PluginViewHost::set_factory() AFTER this and win.
//
// Platform behavior:
//   - Windows (Skia build): registers the native HWND host
//     (plugin_view_host_win.cpp).
//   - Linux (Skia + X11 build): registers the native X11 host
//     (plugin_view_host_linux.cpp).
//   - Apple: no-op (built-in NSView/UIView hosts; no factory needed).
//   - Builds without a platform host: no-op (create() still returns nullptr,
//     but the headless render_to_png/rgba path remains available via Skia).
//
// PluginViewHost::create() calls this automatically on non-Apple platforms
// before consulting the factory, so the foreign-host embed and the VST3/CLAP
// adapters get a working host without the caller doing anything. Exposed
// publicly so a host can force-register early (e.g. before its own
// set_factory()) or in a test.
void register_platform_plugin_view_host();

} // namespace pulp::view
