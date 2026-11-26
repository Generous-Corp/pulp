#pragma once

/// @file inspector_overlay.hpp
/// The inspector's canvas overlay for a Vellum view tree: hover and selection
/// state, the selection box with its handles, the type badge, and the
/// measurements (padding and margin bands, distances to the parent and
/// between views), painted over the inspected root.
///
/// The overlay paints no side panel, so the whole canvas stays live for
/// picking. The inspector window is the tree and property surface; a host
/// mirrors selection between the two through `selected_view` and
/// `set_selected_view`.

#include <pulp/vellum_components/aliases.hpp>
#include <pulp/vellum_components/overlay_geometry.hpp>

#include <vellum/view/input_events.hpp>

namespace pulp::vellum_components {

/// Where to put a badge so it stays readable.
struct BadgePlacement {
    float x = 0.0f;
    float y = 0.0f;
    bool below = false; ///< true when flipped below the selection
};

/// Place a `badge_w` by `badge_h` badge for a selection whose top-left is
/// (`sel_x`, `sel_y`) and whose height is `sel_h`. The badge sits `gap` above
/// the selection; when its top would fall above `top_margin` it flips below
/// the selection. `x` is clamped so the badge never leaves the root, and
/// `root_w <= 0` disables the right-hand clamp.
///
/// Pure arithmetic, so it needs no canvas.
BadgePlacement compute_badge_placement(float sel_x, float sel_y, float sel_h, float badge_w,
                                       float badge_h, float root_w, float gap = 5.0f,
                                       float top_margin = 0.0f);

class InspectorOverlay {
  public:
    /// What follows the pointer: nothing until a click (`follows_focus`), or
    /// the hovered view on every pointer move (`follows_mouse`).
    enum class SelectionMode : std::uint8_t { follows_focus, follows_mouse };

    /// `root` is borrowed and must outlive the overlay.
    explicit InspectorOverlay(View& root);
    ~InspectorOverlay();

    InspectorOverlay(const InspectorOverlay&) = delete;
    InspectorOverlay& operator=(const InspectorOverlay&) = delete;

    /// The view this overlay inspects and paints into. The paint hook compares
    /// it with the root being painted so the overlay never paints into another
    /// surface, such as the inspector window's own.
    View& inspected_root() const {
        return root_;
    }

    bool is_active() const {
        return active_;
    }
    /// Turning the overlay off drops the selection, the hover and the
    /// distance anchor so reopening starts clean.
    void set_active(bool active);
    void toggle() {
        set_active(!active_);
    }

    View* selected_view() const {
        return selected_;
    }
    View* hovered_view() const {
        return hovered_;
    }

    /// Select `view` directly, for a window that shares the selection. Null
    /// clears it. Works whether or not the overlay is active.
    void set_selected_view(View* view) {
        selected_ = view;
    }

    /// Move the selection to its parent. False, and no change, at the root or
    /// with no selection.
    bool select_parent();

    SelectionMode selection_mode() const {
        return selection_mode_;
    }
    void set_selection_mode(SelectionMode mode) {
        selection_mode_ = mode;
    }
    void toggle_selection_mode() {
        selection_mode_ = selection_mode_ == SelectionMode::follows_focus
                              ? SelectionMode::follows_mouse
                              : SelectionMode::follows_focus;
    }

    /// Show the eight resize handles on the selection and the move and
    /// resize cursors over it. Off by default.
    void set_dragging_enabled(bool enabled) {
        dragging_enabled_ = enabled;
    }
    bool dragging_enabled() const {
        return dragging_enabled_;
    }

    /// The affordance of a press at `pos` (root space): a handle resizes, the
    /// body moves, anywhere else, or with dragging off or nothing selected,
    /// none.
    overlay::CursorAffordance cursor_affordance_at(Point pos) const;

    /// `cursor_affordance_at` as the `View::CursorStyle` value a host applies,
    /// or -1 to keep the cursor of the view under the pointer.
    int cursor_style_for(Point pos) const;

    /// Offer a key to the overlay. Cmd+I (Ctrl+I elsewhere) toggles it; while
    /// active, Escape deselects and keeps the overlay on, and M flips the
    /// selection mode. Returns true when the key was consumed.
    bool handle_key_event(const ::vellum::view::KeyEvent& event);

    /// Offer a mouse event, in root space, to the overlay. A press selects the
    /// view under it (Alt+press measures from an anchor) and is consumed; a
    /// plain move only tracks the hover and is not. Returns true when
    /// consumed. Inactive overlays consume nothing.
    bool handle_mouse_event(const ::vellum::view::MouseEvent& event);

    /// Paint the overlay into `canvas`, which is already showing the inspected
    /// root: the highlight, then a line with its pixel distance from the
    /// measurement anchor, and from the Alt-hover target, to the selection,
    /// then the selection's padding and margin bands and its distances to its
    /// parent. Selection and hover that no longer point into the tree are
    /// dropped first. A no-op while inactive.
    void paint(canvas::Canvas& canvas);

    /// The view the Alt-hover measurement currently reaches toward, or null.
    const View* alt_hover_target() const {
        return alt_hover_target_;
    }
    /// The view an Alt+press pinned as the start of a measurement, or null.
    const View* distance_anchor() const {
        return distance_anchor_;
    }

  private:
    void drop_stale_views();
    void paint_highlight(canvas::Canvas& canvas);
    void paint_distance_lines(canvas::Canvas& canvas);
    void paint_box_model(canvas::Canvas& canvas, const View& view);
    bool select_with_mouse(const ::vellum::view::MouseEvent& event, bool is_press,
                           bool is_drag_tick, bool is_release);

    View& root_;
    bool active_ = false;
    View* selected_ = nullptr;
    View* hovered_ = nullptr;
    View* distance_anchor_ = nullptr;
    View* alt_hover_target_ = nullptr;
    SelectionMode selection_mode_ = SelectionMode::follows_focus;
    bool dragging_enabled_ = false;

    static constexpr float kFontSize = 11.0f;
    static constexpr float kEdgeFontSize = 9.0f;
    static constexpr float kBadgeHeight = 16.0f;
    static constexpr float kHandleHalfSide = 4.0f;
};

/// Route the View layer's inspector hooks (paint, key, mouse, cursor) to
/// `inspector`. The hooks are process-wide, so one overlay owns them at a
/// time; a later call replaces the earlier owner. Every hook reads the owner
/// when it fires, so `uninstall_inspector_hooks`, or the owner's destructor,
/// leaves the installed hooks inert rather than dangling. Paint, mouse and
/// cursor events carry the root they belong to and are ignored unless it is
/// the inspected root.
void install_inspector_hooks(InspectorOverlay& inspector);

/// Release every hook slot and forget the owner.
void uninstall_inspector_hooks();

} // namespace pulp::vellum_components
