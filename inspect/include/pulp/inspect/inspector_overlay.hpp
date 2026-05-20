// inspector_overlay.hpp — Visual inspector overlay for Pulp view trees
// Renders highlight, property panel, and frame stats on top of the plugin UI.
// Toggled via Cmd+I (macOS) / Ctrl+I (Windows/Linux).
#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/inspect/editor_url.hpp>
#include <pulp/inspect/source_jump.hpp>

#include <choc/containers/choc_Value.h>

#include <array>
#include <cstddef>
#include <cstdint>
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

    // ── Phase 3a — drag handles ─────────────────────────────────────
    /// Toggle drag-handles mode. When enabled AND a view is selected AND
    /// has an anchor_id, eight 8×8 handles paint at the selected view's
    /// corners + edges; mouse-down on a handle starts a resize gesture
    /// that mutates flex inputs (preferred_width / preferred_height) so
    /// Yoga picks up the new size, then emits "layout.width" /
    /// "layout.height" tweaks on mouse-up. Default OFF for safety —
    /// without it, normal click-to-select is unchanged.
    void set_dragging_enabled(bool enabled) { dragging_enabled_ = enabled; }
    bool dragging_enabled() const { return dragging_enabled_; }
    void toggle_dragging() { dragging_enabled_ = !dragging_enabled_; }

    // ── Phase 6.1 — per-pass GPU/render attribution viewer ──────────
    //
    // Spike reference: planning/2026-05-19-inspector-phase6-gpu-perf-spike.md
    // § Phase 6.1. The RenderPassManager already populates per-pass
    // `PassStats { type, draw_calls, time_ms }` every frame, but only
    // keeps the *last* frame. The attribution viewer accumulates a
    // rolling history (kPassHistoryFrames) per pass type so the panel
    // can show frame-over-frame trend + smoothed averages + a
    // budget-overrun event count.
    //
    // IMPORTANT (honesty about what's measured): `PassStats::time_ms`
    // is CPU wall-time around the pass's draw calls — NOT true GPU
    // time. Real GPU timestamp queries are Phase 6.5 (deferred — see
    // the spike's "What we DON'T have" table). The viewer labels its
    // timings "cpu" so nobody mistakes them for GPU-side numbers.

    /// Aggregated attribution data for a single render pass type,
    /// computed over the rolling frame history.
    struct PassAttribution {
        int type = 0;             ///< RenderPassType cast to int.
        const char* name = "";    ///< Human-readable pass name.
        bool present = false;     ///< Pass appeared in the most recent frame.
        float last_cpu_ms = 0.0f; ///< CPU time_ms in the most recent frame.
        float avg_cpu_ms = 0.0f;  ///< Mean CPU time_ms over recorded history.
        float peak_cpu_ms = 0.0f; ///< Worst CPU time_ms over recorded history.
        int last_draw_calls = 0;  ///< Draw calls in the most recent frame.
        int peak_draw_calls = 0;  ///< Worst draw-call count over history.
        std::size_t samples = 0;  ///< Number of recorded frames for this pass.
    };

    /// Number of frames the rolling per-pass history retains. The spike
    /// asks for a "last 60 frames" trend; 60 keeps one second of 60fps
    /// history without unbounded growth.
    static constexpr std::size_t kPassHistoryFrames = 60;

    /// Sample the attached RenderPassManager's current-frame pass stats
    /// into the rolling history. Safe to call when no RPM is attached
    /// (no-op). Called automatically at the start of every paint() so
    /// the viewer always reflects fresh data; also callable directly
    /// from tests to drive deterministic history without a real frame
    /// loop. Returns true if a frame was actually captured.
    bool capture_pass_frame();

    /// Per-pass attribution computed over the current rolling history,
    /// ordered by RenderPassType (background → post_effects). Entries
    /// with `present == false` had no frames in history and are skipped
    /// by the panel; tests can still inspect the full ordered set.
    std::vector<PassAttribution> pass_attribution() const;

    /// Count of frames in history where the manager reported the frame
    /// exceeded its time budget (RenderPassManager::over_budget()).
    /// Surfaced as the panel's "overruns" figure.
    int budget_overrun_count() const { return budget_overrun_count_; }

    /// Total frames sampled into the rolling history since construction
    /// (saturates conceptually — only the last kPassHistoryFrames worth
    /// of per-pass detail is retained, but this counter keeps growing).
    std::uint64_t pass_frames_captured() const { return pass_frames_captured_; }

    /// Toggle the per-pass attribution panel section. Off by default so
    /// the inspector panel layout is unchanged until the user opts in
    /// (P-key in handle_key_event). When on, the section replaces the
    /// lower portion of the property panel.
    void set_pass_viewer_enabled(bool enabled) { pass_viewer_enabled_ = enabled; }
    bool pass_viewer_enabled() const { return pass_viewer_enabled_; }
    void toggle_pass_viewer() { pass_viewer_enabled_ = !pass_viewer_enabled_; }

    // ── Phase 3c — color eyedropper ─────────────────────────────────
    /// Toggle eyedropper mode. When enabled, mouse-move samples the
    /// color under the cursor and a swatch + hex readout follows the
    /// pointer; the next click captures that color and emits it as a
    /// tweak on the selected view's color property (default
    /// "style.background_color"). Toggled via the E key (no modifier)
    /// while the inspector is active — mirrors how D toggles drag
    /// handles in Phase 3a. Default OFF: without it, normal
    /// click-to-select is unchanged.
    ///
    /// Sampling strategy: if the live Canvas exposes pixel readback
    /// (`Canvas::read_pixels` — Skia raster surfaces only), the
    /// eyedropper reads the exact framebuffer pixel under the cursor.
    /// Otherwise it falls back to the resolved-style color of the
    /// top-most View under the cursor — walking up the tree to the
    /// nearest ancestor with an explicit background color. The v1
    /// fallback is documented as a known simplification.
    void set_eyedropper_active(bool active);
    bool eyedropper_active() const { return eyedropper_active_; }
    void toggle_eyedropper() { set_eyedropper_active(!eyedropper_active_); }

    /// Last color sampled by the eyedropper (updated on every
    /// mouse-move while eyedropper mode is active). Read by tests and
    /// by the cursor-swatch paint. Defaults to transparent black until
    /// the first sample lands.
    Color eyedropper_sample() const { return eyedropper_sample_; }

    /// Whether `eyedropper_sample_` reflects a real sample yet (false
    /// until the first mouse-move inside the canvas while active).
    bool eyedropper_has_sample() const { return eyedropper_has_sample_; }

    /// The dotted color property the eyedropper writes to on click.
    /// Defaults to "style.background_color"; a host could retarget it
    /// to "style.color" / "style.border_color" before a pick. The
    /// setter ignores empty strings.
    const std::string& eyedropper_target() const { return eyedropper_target_; }
    void set_eyedropper_target(std::string path) {
        if (!path.empty()) eyedropper_target_ = std::move(path);
    }

    /// Sample the color under `pos` (root coords) WITHOUT applying it.
    /// Visible for tests + used internally by the mouse-move handler.
    /// Returns true if a color was found (always true for the
    /// resolved-style fallback once a view is under the cursor).
    bool sample_color_at(Point pos, Canvas* canvas, Color& out) const;

    /// Apply the last sampled color as a tweak to the current
    /// selection's `eyedropper_target_` property, encoded as a
    /// "#rrggbb" / "#rrggbbaa" hex string, with source
    /// "inspector-eyedropper". Returns false if there's no sample yet
    /// or `emit_tweak_for_selection` reports no-op (no selection /
    /// anchor / store). On success the eyedropper auto-disables so a
    /// pick is a single, deliberate action.
    bool apply_eyedropper_pick();

    // ── Selection ───────────────────────────────────────────────────
    View* selected_view() const { return selected_; }
    View* hovered_view() const { return hovered_; }

    // ── Phase 5.1 — source-jump ─────────────────────────────────────
    /// The editor-URL config used to format source-jump URLs. The host
    /// sets this once (mirroring the DomainHandler's config); the `J`
    /// hotkey and `jump_to_selection_source()` read it. Defaults to the
    /// built-in VS Code template — see pulp/inspect/editor_url.hpp.
    void set_config(InspectorConfig config) { config_ = std::move(config); }
    const InspectorConfig& config() const { return config_; }

    /// Resolve the selected view's authored source location and (unless
    /// `dry_run`) open the user's editor at that file:line. Returns the
    /// full result so callers / tests can inspect the formatted URL.
    /// A graceful no-op (ok == false) when there is no selection or the
    /// selected view carries no source provenance — the inspector never
    /// throws or spawns a process for a non-imported view.
    SourceJumpResult jump_to_selection_source(bool dry_run = false);

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

    // ── Phase 2.5 — tweak management panel (Photoshop-layers style) ─
    //
    // A scrollable panel listing every tweak in the attached
    // TweakStore, grouped by anchor. Each tweak row carries three
    // per-tweak controls: bypass (eye), lock, and delete. Toggled with
    // the `T` key while the inspector is active. Default OFF so the
    // pre-2.5 inspector layout is unchanged until the user opts in.
    void set_tweaks_panel_visible(bool visible) { tweaks_panel_visible_ = visible; }
    bool tweaks_panel_visible() const { return tweaks_panel_visible_; }
    void toggle_tweaks_panel() { tweaks_panel_visible_ = !tweaks_panel_visible_; }

    /// Number of tweak rows currently laid out in the management panel
    /// (populated by the most recent paint() while the panel is
    /// visible). Visible for tests so they can assert the panel
    /// rendered the expected number of rows without scraping pixels.
    std::size_t tweak_row_count() const { return tweak_rows_.size(); }

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

    // ── Phase 2.5 — tweak management panel state ────────────────────
    //
    // The panel is laid out during paint_tweaks_section(); the row
    // hit-rects are stashed in tweak_rows_ so the SAME-frame mouse
    // handler can resolve a click to a (anchor, path, action). Each
    // row carries three 16×16 icon hit-rects: bypass / lock / delete.
    bool tweaks_panel_visible_ = false;
    float tweaks_scroll_y_ = 0.0f;
    enum class TweakAction : std::uint8_t { none, bypass, lock, remove };
    struct TweakRow {
        std::string anchor_id;     ///< Owning anchor
        std::string property_path; ///< Dotted path of this tweak
        Rect bypass_icon;          ///< Hit-rect, root coords
        Rect lock_icon;
        Rect delete_icon;
    };
    std::vector<TweakRow> tweak_rows_;

    // Tree expansion: collapsed nodes
    std::unordered_set<const View*> collapsed_;

    // Optional stats source
    render::RenderPassManager* rpm_ = nullptr;

    // ── Phase 6.1 — per-pass attribution history ────────────────────
    //
    // The RenderPassManager forgets everything but the current frame,
    // so the viewer keeps its own rolling buffers. There are exactly 5
    // RenderPassType values; each gets a fixed-size ring of CPU-time +
    // draw-call samples. Indexing the outer array by RenderPassType
    // (cast to size_t) keeps capture O(passes) with no allocation.
    static constexpr std::size_t kPassTypeCount = 5;
    struct PassRing {
        std::array<float, kPassHistoryFrames> cpu_ms{};
        std::array<int, kPassHistoryFrames> draw_calls{};
        std::size_t count = 0;  ///< Valid samples (caps at kPassHistoryFrames).
        std::size_t head = 0;   ///< Next write index (wraps).
        // Whether this pass type appeared in the most recently captured
        // frame — distinguishes "no history" from "history but absent
        // this frame" so the panel can dim a pass that just went quiet.
        bool present_last_frame = false;
    };
    std::array<PassRing, kPassTypeCount> pass_rings_{};
    int budget_overrun_count_ = 0;
    std::uint64_t pass_frames_captured_ = 0;
    // Last frame_count() observed from the RPM — capture is a no-op if
    // the manager hasn't advanced a frame, so multiple paints of the
    // same frame don't inflate the history with duplicates.
    std::uint64_t last_captured_frame_ = 0;
    bool pass_viewer_enabled_ = false;

    // Phase 0b PR-C-1: optional in-process gesture-tweak persistence.
    // When null, emit_tweak_for_selection() is a no-op.
    TweakStore* tweak_store_ = nullptr;

    // Phase 5.1: editor-URL config for the `J` source-jump hotkey.
    // Defaults to the built-in VS Code template; the env override
    // (PULP_INSPECTOR_EDITOR_URL) still applies at jump time.
    InspectorConfig config_{};

    // Phase 3a — drag-handles state. Off by default so the inspector
    // behaves identically to the pre-3a build until the user opts in
    // (D-key toggle in handle_key_event).
    bool dragging_enabled_ = false;
    enum class DragCorner : std::uint8_t { none, nw, ne, sw, se };
    DragCorner active_drag_ = DragCorner::none;
    Point drag_start_pos_{};         // mouse pos when drag began (root coords)
    Rect drag_start_bounds_{};       // selected_->bounds() snapshot
    float drag_start_pref_w_ = 0.0f; // flex().preferred_width snapshot
    float drag_start_pref_h_ = 0.0f; // flex().preferred_height snapshot

    // Returns the corner whose 8px handle hit-rectangle contains `pos`,
    // for the currently-selected view (root coords). Returns
    // DragCorner::none if no handle is hit or no view selected.
    DragCorner hit_test_drag_handle(Point pos) const;

    // ── Phase 3c — color eyedropper state ───────────────────────────
    // Off by default so the inspector behaves identically to the
    // pre-3c build until the user opts in (E-key toggle).
    bool eyedropper_active_ = false;
    bool eyedropper_has_sample_ = false;
    Color eyedropper_sample_{0.0f, 0.0f, 0.0f, 0.0f};
    Point eyedropper_cursor_{};  ///< Last cursor pos (root coords) for swatch.
    std::string eyedropper_target_ = "style.background_color";

    // Walk from the View under `pos` up to the nearest ancestor that
    // has an explicit background color; returns that color. Returns
    // false when no view under the cursor carries a background — the
    // resolved-style fallback used when Canvas pixel readback is
    // unavailable.
    bool resolved_color_under(Point pos, Color& out) const;

    // Paint the cursor-following swatch + hex readout. No-op unless
    // eyedropper mode is active and at least one sample has landed.
    void paint_eyedropper_cursor(Canvas& canvas);

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
    /// Phase 6.1 — render the per-pass attribution viewer in the panel
    /// region normally occupied by the property section. Each pass type
    /// gets a row with a color-coded type bar, last/avg/peak CPU time,
    /// draw-call counts, and a 60-frame sparkline trend.
    void paint_pass_attribution(Canvas& canvas, float x, float y, float w, float h);

    // Phase 2.5 — render the tweak management panel into the given
    // rect. Repopulates tweak_rows_ with this frame's hit-rects.
    void paint_tweaks_section(Canvas& canvas, float x, float y, float w, float h);

    // ── Panel hit testing ───────────────────────────────────────────
    bool point_in_panel(Point p) const;
    const TreeItem* tree_item_at_y(float panel_y) const;

    // Phase 2.5 — resolve a root-coord point to a tweak-row icon hit.
    // Sets `out_row` to the matching row index and returns the action;
    // returns TweakAction::none (out_row untouched) on a miss. Called
    // by handle_mouse_event() before the tree-selection fallthrough.
    TweakAction tweak_action_at(Point p, std::size_t& out_row) const;

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
