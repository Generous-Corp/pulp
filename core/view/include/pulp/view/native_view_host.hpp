#pragma once

#include <pulp/view/view.hpp>

#include <cstdint>
#include <functional>
#include <vector>

namespace pulp::view {

class WindowHost;
class PluginViewHost;

// Platform-native child-view handle (NSView* / UIView* / HWND / X11 Window),
// mirroring the identical alias in plugin_view_host.hpp / web_view.hpp. Declared
// locally so this header stays light (it does not pull in the full host
// headers); a repeated `using` to the same type is a legal, harmless
// redeclaration when those headers are also included.
using NativeViewHandle = void*;

/// A Yoga-laid-out widget that embeds a single platform-native child view (a
/// system WebView, a native text field with IME + OS accessibility, a video /
/// camera layer, an OS picker) inside Pulp's GPU-painted view tree.
///
/// Pulp paints its UI to a GPU Skia/Dawn surface; a native child view is a real
/// OS view composited by the window server. `NativeViewHost` is the layout-owned
/// seam between the two: it takes a native handle you supply
/// (`WebViewPanel::native_handle()`, an `NSTextField`, an `AVPlayerLayer`-backed
/// view, …) and drives that view's frame from the widget's computed Yoga layout —
/// so the native view sits, moves, resizes, tracks scroll, and clips to its
/// scroll/clip ancestor like any other widget. It wraps the host primitives
/// `attach_native_child_view` / `set_native_child_view_bounds` /
/// `set_native_child_view_clip` / `detach_native_child_view` (real on macOS + iOS;
/// a no-op that returns `false` on Windows / Linux / Android today).
///
/// ── Z-ORDER (fixed, by design) ───────────────────────────────────────────────
/// A native child ALWAYS composites ABOVE the entire GPU layer of the same host
/// view — the OS window server draws it over Pulp's Skia surface. A GPU-painted
/// widget can NOT paint over a native child. This makes `NativeViewHost` suited
/// to full-region embeds (a WebView panel, a video pane) rather than a native
/// control floating mid-canvas beneath other GPU widgets. There is no per-widget
/// z-index against native content; do not rely on painting Pulp chrome on top of
/// the embedded native view.
///
/// ── CLIPPING ─────────────────────────────────────────────────────────────────
/// The widget computes the visible sub-rectangle of the native child by
/// intersecting its absolute frame with every clipping ancestor (a `ScrollView`
/// viewport, or a `View` with `overflow: hidden`) and asks the host to mask the
/// native view to that rect — so a native child inside a scroll region does not
/// spill past the viewport edges. Masking preserves the child's content size
/// (it does not reflow a WebView by resizing it). Ancestor CSS transforms
/// (rotate / scale / matrix) on an ancestor are NOT reflected in the native
/// child's frame — only translation and scroll are — because an OS view can't be
/// cheaply arbitrary-transformed; keep native embeds under translate/scroll
/// ancestry.
///
/// ── HEADLESS CAPTURE ─────────────────────────────────────────────────────────
/// A native overlay is invisible to `render_to_png` (it isn't painted into the
/// Skia canvas). This widget sets `contains_native_overlay(true)` whenever a
/// native child is set, and forwards `capture_native_overlay_png()` to the
/// snapshot callback you supply (e.g. `WebViewPanel::snapshot_png()`), so the
/// smart-capture path can composite the overlay instead of refusing with a blank
/// frame. Without a snapshot callback, capture honestly refuses.
///
/// ── LIFECYCLE ────────────────────────────────────────────────────────────────
/// Attachment follows the host back-reference, not `on_attached()`: the native
/// view is attached during the first `paint_all()` after both a host and a
/// handle are present, re-driven each frame, and detached when the host is
/// removed (`set_window_host(nullptr)` / `set_plugin_view_host(nullptr)`), when
/// the handle changes, or in the destructor (the backstop — `on_detached()` is
/// not fired for deeply nested nodes on host teardown).
class NativeViewHost : public View {
public:
    /// Returns a PNG-encoded snapshot of the native child at the requested pixel
    /// size, or empty bytes when unavailable / not ready. Called on the main
    /// thread by the headless-capture path.
    using SnapshotFn = std::function<std::vector<uint8_t>(uint32_t width,
                                                          uint32_t height)>;

    NativeViewHost() = default;
    ~NativeViewHost() override;

    /// Supply the native child view to embed. `handle` is a platform view handle
    /// (NSView* / UIView* / HWND / …), typically `WebViewPanel::native_handle()`.
    /// `snapshot` is an optional headless-capture callback (e.g. a lambda that
    /// returns `WebViewPanel::snapshot_png()`). Passing a non-null handle marks
    /// the subtree as containing a native overlay. Re-driving with a different
    /// handle detaches the previous child first. Call on the main thread.
    void set_native_child(NativeViewHandle handle, SnapshotFn snapshot = {});

    /// Detach and forget the native child (does not destroy the underlying OS
    /// view — ownership stays with the caller / the WebViewPanel).
    void clear_native_child();

    /// The currently-set native handle (null if none).
    NativeViewHandle native_child() const { return handle_; }

    /// True while the native child is embedded in a host. False before the first
    /// attach, on unsupported platforms (host attach returned false), and after
    /// detach.
    bool is_native_attached() const { return attached_; }

    // ── Introspection (headless tests) ──────────────────────────────────────
    /// The native child's absolute frame in root/host coordinates as last
    /// computed from Yoga layout (top-left origin).
    Rect computed_child_frame() const { return last_frame_; }
    /// The last visible sub-rectangle in root/host coordinates after intersecting
    /// with clipping ancestors. Equal to `computed_child_frame()` when nothing
    /// clips it; empty when fully scrolled out.
    Rect computed_visible_rect() const { return last_visible_; }
    /// True when the native child is trimmed by a clipping ancestor (the visible
    /// rect is a strict subset of the frame).
    bool is_clipped() const { return last_clipped_; }
    /// Recompute geometry and push it to the host now (also attaches lazily when
    /// a host + handle are present). Normally driven by `paint_all()`; exposed so
    /// headless tests can drive it without a canvas.
    void update_native_layout();

    // ── View overrides ──────────────────────────────────────────────────────
    std::vector<uint8_t> capture_native_overlay_png(uint32_t width,
                                                     uint32_t height) override;
    void paint_all(canvas::Canvas& canvas) override;
    void set_window_host(WindowHost* host) override;
    void set_plugin_view_host(PluginViewHost* host) override;

private:
    enum class HostKind { none, plugin, window };

    /// Compute the child's absolute frame and its visible (clip-intersected)
    /// rect from the current parent chain. Pure geometry; no host calls.
    void compute_geometry(Rect& frame, Rect& visible, bool& clipped) const;

    /// Attach to the active host if not yet attached and a handle is present.
    bool try_attach();
    /// Detach from whichever host we attached to (uses the stored raw pointer,
    /// valid even while the host is nulling its back-reference at teardown).
    void detach_from_host();
    /// Detach if the host we attached to is no longer the active host.
    void reconcile_host();
    /// Push frame + clip to the host if either changed since the last push.
    void push_geometry(const Rect& frame, const Rect& visible, bool clipped);

    NativeViewHandle handle_ = nullptr;
    SnapshotFn snapshot_;

    bool attached_ = false;
    HostKind attached_kind_ = HostKind::none;
    PluginViewHost* attached_plugin_ = nullptr;
    WindowHost* attached_window_ = nullptr;

    // Last computed geometry (introspection getters; kept current every layout
    // pump regardless of platform / attach state).
    Rect last_frame_{};
    Rect last_visible_{};
    bool last_clipped_ = false;

    // Last geometry actually pushed to the host, to skip redundant per-frame
    // host calls.
    Rect pushed_frame_{};
    Rect pushed_visible_{};
    bool pushed_clipped_ = false;
    bool have_pushed_ = false;
};

} // namespace pulp::view
