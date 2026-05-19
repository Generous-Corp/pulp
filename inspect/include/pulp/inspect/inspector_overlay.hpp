// inspector_overlay.hpp — Visual inspector overlay for Pulp view trees
// Renders highlight, property panel, and frame stats on top of the plugin UI.
// Toggled via Cmd+I (macOS) / Ctrl+I (Windows/Linux).
#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/canvas/canvas.hpp>

#include <choc/containers/choc_Value.h>

#include <functional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pulp::render { class RenderPassManager; }

namespace pulp::inspect {

class TweakStore;

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

    /// Phase 0b PR-C-1 — connect the inspector overlay to the in-process
    /// TweakStore so gesture detectors can persist direct-manipulation
    /// edits without round-tripping through the TCP protocol. The
    /// protocol path (Inspector.applyTweak) still works independently;
    /// this is the FAST in-process path for overlay-driven gestures.
    /// Setting nullptr disables the in-process emission (e.g. when no
    /// TweakStore is wired into the host).
    void set_tweak_store(TweakStore* store) { tweak_store_ = store; }
    TweakStore* tweak_store() const { return tweak_store_; }

    /// Emit a tweak for the currently-selected view's anchor. Returns
    /// false if there's no selection, the selection has no anchor_id,
    /// or no TweakStore is attached. Used by gesture-detection code in
    /// the overlay (drag-handles, color-picker, field-edit) — Phase 3a
    /// builds on this with actual UI; PR-C-1 ships only the data path.
    bool emit_tweak_for_selection(std::string_view property_path,
                                  choc::value::Value value,
                                  std::string_view source = "inspector-gesture");

    // ── Selection ───────────────────────────────────────────────────
    View* selected_view() const { return selected_; }
    View* hovered_view() const { return hovered_; }

    // ── Phase 3b — live-editable box-model fields ──────────────────
    //
    // Public read-only accessors for the editing state — used by tests
    // and by host-side cursor hints. The setter side is internal: edit
    // mode is entered by clicking on a numeric value in the property
    // panel and exited via Enter / Esc / Tab / blur.
    //
    // `editing_field()` returns the dotted property path of the field
    // currently being edited (e.g. "layout.padding"), or an empty
    // string when not editing. `edit_buffer()` is the live text the
    // user has typed so far (digits + optional sign / decimal); empty
    // until at least one character is entered. `edit_caret_pos()` is
    // the 0-based caret index into `edit_buffer()`.
    const std::string& editing_field() const { return editing_field_; }
    const std::string& edit_buffer() const { return edit_buffer_; }
    std::size_t edit_caret_pos() const { return edit_caret_pos_; }
    bool is_editing() const { return !editing_field_.empty(); }

    /// Begin editing the named property on the current selection. The
    /// `initial_value` is what shows in the buffer at edit start and
    /// is restored on Esc-cancel. Returns false if there's no
    /// selection (nothing to edit) or `field_path` is empty.
    /// Visible for tests; production code enters edit mode via the
    /// panel-click path in handle_mouse_event().
    bool begin_field_edit(std::string field_path, float initial_value);

    /// Commit the current edit buffer as a tweak, then exit edit mode.
    /// Returns true if a tweak was emitted (selection has an anchor +
    /// a TweakStore is wired); returns false if commit is a no-op
    /// (selection missing anchor, no store, no field being edited).
    /// Regardless of return value, edit mode is exited.
    bool commit_field_edit();

    /// Cancel the current edit without writing a tweak. Restores the
    /// original value to the underlying View when changes were applied
    /// optimistically (none yet — Phase 3b commits only on Enter).
    void cancel_field_edit();

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

    // ── Phase 3b — editable-field state ─────────────────────────────
    //
    // editing_field_ doubles as the "currently editing" flag and the
    // dotted property path (e.g. "layout.padding"). Empty = not
    // editing. edit_buffer_ holds the live numeric text the user has
    // typed; edit_caret_pos_ is a 0-based index into it. The original
    // value is captured at edit-begin time so Esc can revert without
    // re-querying the View (which may have been mutated by Yoga
    // reflow already).
    std::string editing_field_;
    std::string edit_buffer_;
    std::size_t edit_caret_pos_ = 0;
    float edit_original_value_ = 0.0f;
    View* edit_target_view_ = nullptr;  ///< Owner of the editing field.

    // Ordered list of editable fields populated during paint_props_section()
    // — used (a) for Tab to walk forward, and (b) for click hit-testing.
    struct EditableField {
        std::string path;   ///< Dotted property path, e.g. "layout.padding"
        Rect bounds;        ///< Hit-test rect in root (window) coordinates
        float value;        ///< Current numeric value (for display + edit-begin)
    };
    std::vector<EditableField> editable_fields_;

    // Tree expansion: collapsed nodes
    std::unordered_set<const View*> collapsed_;

    // Optional stats source
    render::RenderPassManager* rpm_ = nullptr;

    // Phase 0b PR-C-1: optional in-process gesture-tweak persistence.
    // When null, emit_tweak_for_selection() is a no-op.
    TweakStore* tweak_store_ = nullptr;

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

    // ── Phase 3b — editable-field helpers ───────────────────────────
    /// Returns the index in editable_fields_ at the given root-coord
    /// point, or -1 if the point doesn't hit any field rect.
    int editable_field_at(Point p) const;
    /// Process a key event while edit mode is active. Returns true if
    /// the event was consumed by the editor (digits, navigation, etc).
    bool handle_edit_key(const KeyEvent& event);
    /// Apply the live edit buffer's numeric value to the underlying
    /// View input (e.g. flex().padding = N). No persistence; used for
    /// real-time preview as the user types / arrows.
    void apply_edit_buffer_to_view();
    /// Read the current numeric value of `field_path` off the
    /// selected_ view. Returns 0 if path is unknown / no selection.
    float read_field_value(std::string_view field_path) const;
    /// Write `value` to `field_path` on the selected_ view, triggering
    /// invalidate_layout() so Yoga reflows next paint pass.
    void write_field_value(std::string_view field_path, float value);

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
