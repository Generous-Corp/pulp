#pragma once

/// @file popup_window.hpp
/// A borderless popup window the client draws into: combo lists and other
/// popups that must extend past an editor's edges and look like the product
/// rather than a system menu.
///
/// The popup is anchored to a rectangle in the editor view's own coordinates
/// and placed on that view's screen, below or above the anchor. It belongs to
/// the editor's window (it moves with it and stays above it) and closes on a
/// click outside it (the click is consumed, so it does not reach the editor),
/// on Escape, when the application or the popup loses focus, when the parent
/// window closes, or when the client closes it. Its content view is a plain
/// layer-backed view; attach a native surface or any other view to it.

#include <pulp/platform/main_thread.hpp>
#include <pulp/platform/native_handle.hpp>

#include <functional>
#include <memory>

namespace pulp::platform {

/// A rectangle in points.
struct PopupRect {
    float x = 0, y = 0, width = 0, height = 0;
};

enum class PopupPlacement {
    below,
    above,
    /// Below when it fits on the screen, else above when that fits, else the
    /// side with more room.
    auto_flip,
};

enum class PopupDismissReason {
    client,        ///< PopupWindow::close()
    outside_click, ///< a click outside the popup, in this or another app
    escape,        ///< Escape while the popup has keyboard focus
    /// Key moved to a window other than the editor's, or the application
    /// lost focus. Key handed back to the editor's own window with no click
    /// (which activating the application can do) does not count: the popup
    /// takes key again.
    focus_lost,
    parent_closed, ///< the editor's window is closing
};

/// Short stable name ("client", "outside_click", ...).
const char* to_string(PopupDismissReason reason) noexcept;

struct PopupWindowOptions {
    /// The editor view the popup belongs to (an NSView* on macOS).
    NativeViewHandle view = nullptr;
    /// The anchor, in the view's own points with the origin at its top-left.
    PopupRect anchor;
    /// Popup size in points. It is never narrower than the anchor.
    float width = 0, height = 0;
    PopupPlacement placement = PopupPlacement::auto_flip;
    /// Take keyboard focus (needed for keyboard navigation in a list). The
    /// application is not activated either way.
    bool takes_key_focus = true;
};

/// Where a popup goes, in top-left based screen points: `anchor` and
/// `visible` (the usable area of the anchor's screen) in the same space.
/// Pure arithmetic, exposed for tests and for clients that pre-size content.
PopupRect place_popup(const PopupRect& anchor, float width, float height, const PopupRect& visible,
                      PopupPlacement placement);

class PopupWindow {
  public:
    /// Open a popup for `options.view`. Returns nullptr when the view is not
    /// in a window or the platform has no popup windows. `on_dismiss` runs
    /// once, on the main thread, when the popup closes for any reason but its
    /// owner going away: destroying the PopupWindow or the MainThreadScope
    /// closes it without a report. Main thread only.
    static std::unique_ptr<PopupWindow> open(MainThreadScope& scope,
                                             const PopupWindowOptions& options,
                                             std::function<void(PopupDismissReason)> on_dismiss);

    ~PopupWindow();
    PopupWindow(const PopupWindow&) = delete;
    PopupWindow& operator=(const PopupWindow&) = delete;

    /// The view to draw into (an NSView* on macOS), sized to the popup.
    NativeViewHandle content_view() const;
    /// The popup's window (an NSWindow* on macOS).
    NativeWindowHandle native_window() const;
    /// Resize and place again against the same anchor.
    void set_size(float width, float height);
    /// Close and report PopupDismissReason::client.
    void close();
    bool is_open() const;
    /// The popup's frame in top-left based screen points.
    PopupRect screen_frame() const;
    /// Device pixels per point of the popup's screen.
    float backing_scale() const;

    struct Impl;

  private:
    explicit PopupWindow(std::shared_ptr<Impl> impl);
    std::shared_ptr<Impl> impl_;
};

} // namespace pulp::platform
