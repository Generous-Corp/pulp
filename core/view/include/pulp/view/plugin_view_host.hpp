#pragma once

#include <pulp/view/view.hpp>
#include <memory>
#include <cstdint>
#include <functional>
#include <vector>

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
    // Platform support (#299):
    //   - macOS: NSView-backed impl in plugin_view_host_mac.mm, including
    //     native child-view attach/bounds/detach.
    //   - iOS: UIView-backed impl in plugin_view_host_ios.mm, including
    //     native child-view attach/bounds/detach.
    //   - Windows/Linux/Android: no built-in impl — host app registers a
    //     factory via set_factory(). Without a factory, create() returns
    //     nullptr explicitly.
    static std::unique_ptr<PluginViewHost> create(View& root, Size size);
    static std::unique_ptr<PluginViewHost> create(View& root, const Options& options);

    // Host-registered factory (#299). Installed by the host app's
    // platform layer on non-Apple targets.
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

    // Attach this view to a parent native view (the DAW's editor window)
    virtual void attach_to_parent(NativeViewHandle parent) = 0;

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

    // Inverse-map a host-space point (e.g. mouse coords in host logical
    // pixels) into root-view space using the active design-viewport
    // transform. Identity when no design viewport is set. Exposed so
    // native event handlers AND unit tests can exercise the same
    // inverse-transform path without needing AppKit / UIKit event
    // delivery.
    virtual Point window_to_root_point(Point pt) const { return pt; }
};

} // namespace pulp::view
