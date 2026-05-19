// inspector_overlay.hpp — Visual inspector overlay for Pulp view trees
// Renders highlight, property panel, and frame stats on top of the plugin UI.
// Toggled via Cmd+I (macOS) / Ctrl+I (Windows/Linux).
#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/canvas/canvas.hpp>

#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace pulp::render { class RenderPassManager; }

namespace pulp::inspect {

using namespace pulp::view;
using namespace pulp::canvas;

class InspectorOverlay {
public:
    explicit InspectorOverlay(View& root);

    // ── Toggle ──────────────────────────────────────────────────────
    bool is_active() const { return active_; }
    void set_active(bool active);
    void toggle() { set_active(!active_); }

    // ── Input interception ──────────────────────────────────────────
    // Call from host BEFORE dispatching to view tree.
    // Returns true if the inspector consumed the event.
    bool handle_key_event(const KeyEvent& event);
    bool handle_mouse_event(const MouseEvent& event);

    // ── Painting ────────────────────────────────────────────────────
    // Call AFTER View::paint_overlays() to paint on top of everything.
    void paint(Canvas& canvas);

    // ── Data sources ────────────────────────────────────────────────
    void set_render_pass_manager(render::RenderPassManager* rpm) { rpm_ = rpm; }

    // ── Selection ───────────────────────────────────────────────────
    View* selected_view() const { return selected_; }
    View* hovered_view() const { return hovered_; }

private:
    View& root_;
    bool active_ = false;

    View* selected_ = nullptr;
    View* hovered_ = nullptr;

    // Distance measurement mode
    View* distance_anchor_ = nullptr;

    // Phase 3f — Alt-hover sibling distance (Figma-style spacing reveal).
    // Tracks the View under the cursor while Alt is held; cleared as soon
    // as Alt is released or the cursor leaves the view area. Distinct from
    // distance_anchor_ (which is Alt+click sticky-anchor mode).
    View* alt_hover_target_ = nullptr;

    // Panel layout
    float panel_width_ = 300.0f;
    float tree_scroll_y_ = 0.0f;

    // Tree expansion: collapsed nodes
    std::unordered_set<const View*> collapsed_;

    // Optional stats source
    render::RenderPassManager* rpm_ = nullptr;

    // ── Flat tree for rendering ─────────────────────────────────────
    struct TreeItem {
        const View* view;
        int depth;
    };
    std::vector<TreeItem> flat_tree_;
    void rebuild_flat_tree();

    // ── Coordinate helpers ──────────────────────────────────────────
    Rect view_bounds_in_root(const View* v) const;

    // ── Paint helpers ───────────────────────────────────────────────
    void paint_highlight(Canvas& canvas);
    void paint_distance_lines(Canvas& canvas);
    void paint_box_model(Canvas& canvas, const View* v);
    void paint_panel(Canvas& canvas);
    void paint_tree_section(Canvas& canvas, float x, float y, float w, float& cursor_y);
    void paint_props_section(Canvas& canvas, float x, float y, float w, float h);
    void paint_stats_bar(Canvas& canvas, float x, float y, float w);

    // ── Panel hit testing ───────────────────────────────────────────
    bool point_in_panel(Point p) const;
    const TreeItem* tree_item_at_y(float panel_y) const;

    // ── Constants ───────────────────────────────────────────────────
    static constexpr float kRowHeight = 20.0f;
    static constexpr float kIndent = 16.0f;
    static constexpr float kFontSize = 11.0f;
    static constexpr float kStatsBarHeight = 24.0f;
};

/// Global inspector instance for the current window.
/// Set by the host when creating the inspector. The platform WindowHost
/// checks this to intercept key/mouse events before normal dispatch.
/// Thread-safe: only accessed on the UI thread.
inline InspectorOverlay* g_active_inspector = nullptr;

/// Install the inspector into the View system's paint and input hooks.
/// Call this after creating the InspectorOverlay and setting g_active_inspector.
/// Installs paint hook, key intercept, and mouse intercept via function pointers
/// so pulp-view doesn't need to link pulp-inspect.
void install_inspector_hooks(InspectorOverlay& inspector);

} // namespace pulp::inspect
