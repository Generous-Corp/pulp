// inspector_overlay.hpp — Visual inspector overlay for Pulp view trees
// Renders highlight, property panel, and frame stats on top of the plugin UI.
// Toggled via Cmd+I (macOS) / Ctrl+I (Windows/Linux).
#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/input_events.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/inspect/editor_url.hpp>
#include <pulp/inspect/source_jump.hpp>
#include <pulp/inspect/tweak_store.hpp>

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
namespace pulp::render { class AtlasInventory; }

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

    /// Phase 6.2 — attach the render layer's texture-atlas inventory so
    /// the atlas viewer tab (`A` hotkey) can report per-atlas
    /// dimensions, page count, and occupancy. The inventory is a
    /// value-snapshot collection owned by the render/host layer; the
    /// overlay only reads it. Passing nullptr (the default) makes the
    /// atlas tab render its "GPU atlas unavailable" empty state — the
    /// inspector never crashes when no GPU atlas is wired.
    void set_atlas_inventory(const render::AtlasInventory* inv) {
        atlas_inventory_ = inv;
    }
    const render::AtlasInventory* atlas_inventory() const {
        return atlas_inventory_;
    }

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

    // ── Phase 2 — drift drawer ──────────────────────────────────────
    //
    // A collapsible panel that lists tweaks whose anchor_id no longer
    // resolves to any live view (orphaned) or whose property path no
    // longer maps (drifted). It surfaces the silent-failure case where
    // a design re-import changed an element so a stored tweak quietly
    // stops applying. The drawer auto-expands the first time drift is
    // detected after a re-import; the user can collapse it with the
    // header chevron.

    /// Recompute the drift list by diffing the attached TweakStore
    /// against the current live view tree's anchor set. A no-op (and
    /// clears the list) when no TweakStore is wired. Called
    /// automatically on first paint after set_active(true); call it
    /// explicitly after a re-import to refresh the drawer.
    void refresh_drift();

    /// Number of drifted/orphaned tweaks from the last refresh_drift().
    std::size_t drift_count() const { return drifted_.size(); }

    /// Whether the drift drawer is currently expanded.
    bool drift_drawer_open() const { return drift_drawer_open_; }
    void set_drift_drawer_open(bool open) { drift_drawer_open_ = open; }
    void toggle_drift_drawer() { drift_drawer_open_ = !drift_drawer_open_; }

    /// Read-only access to the last computed drift list — for tests and
    /// host-side badges.
    const std::vector<TweakStore::DriftedTweak>& drifted() const {
        return drifted_;
    }

    // ── Phase 5.2 — reconciliation tab ──────────────────────────────
    //
    // Spec: planning/2026-05-19-inspector-phase5-source-jump-spike.md
    // § Phase 5.2 + planning/2026-05-18-inspector-direct-manipulation-
    // roadmap.md "Lock to source" / "Drift".
    //
    // The reconciliation tab answers one question per tweak: "will this
    // edit survive a re-import?" It builds on Phase 4a (lock-to-source)
    // and Phase 5.1 (source-jump) — it is a *read-only* report over the
    // existing TweakStore + live view tree, inventing no parallel data
    // model. Every stored tweak is classified into exactly one of three
    // reconciliation states:
    //
    //   * locked-to-source — the tweak's anchor is in the TweakStore
    //     lock set AND still resolves to a live view. It has been (or
    //     will be) promoted into the authored source, so it survives a
    //     fresh re-import. This is the "reconciled" state.
    //   * drifted — the tweak's anchor resolves to a live view but the
    //     anchor is NOT locked. The edit lives only in the runtime
    //     tweak layer (`pulp-tweaks.json`); a re-import that regenerates
    //     the element would not carry it. "Live-only, not written back."
    //   * unresolvable — no live view carries the tweak's anchor_id
    //     (orphaned, per TweakStore::DriftReason::anchor_not_found). The
    //     inspector cannot tell where this edit belongs; locking it is
    //     impossible until the anchor is re-established. Conservative
    //     fallback — never crash, never guess.
    //
    // The tab toggles with the `R` key (reconcile) and, when on, takes
    // over the property-panel region exactly like the Phase 6.1 pass
    // viewer does — the tree section above stays put.

    /// Reconciliation state of a single stored tweak.
    enum class ReconcileStatus : std::uint8_t {
        locked_to_source,  ///< Anchor locked + resolves — survives re-import.
        drifted,           ///< Resolves but unlocked — runtime-layer only.
        unresolvable,      ///< Anchor not found in the live tree (orphaned).
    };

    /// Stringify a ReconcileStatus for the panel + tests.
    static const char* reconcile_status_str(ReconcileStatus status);

    /// One classified tweak row in the reconciliation report.
    struct ReconcileRow {
        std::string anchor_id;
        std::string property_path;
        ReconcileStatus status = ReconcileStatus::unresolvable;
    };

    /// Aggregate reconciliation report over every tweak in the attached
    /// TweakStore, classified against the current live view tree. The
    /// per-status counts are convenience totals over `rows`.
    struct ReconcileReport {
        std::vector<ReconcileRow> rows;
        std::size_t locked_count = 0;
        std::size_t drifted_count = 0;
        std::size_t unresolvable_count = 0;

        std::size_t total() const { return rows.size(); }
    };

    /// Recompute the reconciliation report by classifying every stored
    /// tweak against the live view tree's anchor + lock state. A no-op
    /// returning an empty report when no TweakStore is wired. Pure
    /// computation — safe for tests, never spawns a process, never
    /// throws. Called automatically on first paint while the tab is
    /// open; callers may invoke it directly after a re-import.
    ReconcileReport reconcile_report() const;

    /// Toggle the reconciliation tab. Off by default so the inspector
    /// panel layout is unchanged until the user opts in (`R` key in
    /// handle_key_event). When on, the tab replaces the property
    /// section, mirroring the Phase 6.1 pass viewer.
    void set_reconcile_tab_visible(bool visible) {
        reconcile_tab_visible_ = visible;
    }
    bool reconcile_tab_visible() const { return reconcile_tab_visible_; }
    void toggle_reconcile_tab() {
        reconcile_tab_visible_ = !reconcile_tab_visible_;
    }

    /// Number of tweak rows the reconciliation tab laid out on the most
    /// recent paint() while it was visible. Visible for tests so they
    /// can assert the tab rendered the expected rows without scraping
    /// pixels. Zero when the tab is hidden or no TweakStore is wired.
    std::size_t reconcile_row_count() const {
        return reconcile_rows_.size();
    }

    // ── Phase 6.2 — texture atlas viewer ────────────────────────────
    //
    // Spec: planning/2026-05-19-inspector-phase6-gpu-perf-spike.md
    // § Phase 6.2. The render layer packs small bitmaps into a handful
    // of shelf-packed GPU texture atlases — the glyph atlas (SDF text),
    // the image atlas, the gradient ramp atlas, the path atlas. When an
    // atlas fills, rendering thrashes (evict + re-pack churn). The
    // atlas viewer answers "is my SDF atlas thrashing?" without leaving
    // the inspector.
    //
    // It is a *read-only* report over a render::AtlasInventory — a
    // value-snapshot collection the host wires in via
    // set_atlas_inventory(). The tab invents no parallel data model:
    // every per-atlas figure (dimensions, page count, occupancy, entry
    // count) comes straight from the Pulp-owned atlas classes'
    // introspection accessors. When no inventory is attached the tab
    // renders a graceful "GPU atlas unavailable" empty state.
    //
    // The tab toggles with the `A` key (atlas) and, when on, takes over
    // the property-panel region exactly like the Phase 6.1 pass viewer
    // and Phase 5.2 reconciliation tab — the tree section above stays
    // put so the user keeps navigation context.

    /// Toggle the texture-atlas viewer tab. Off by default so the
    /// inspector panel layout is unchanged until the user opts in
    /// (`A` key in handle_key_event). When on, the tab replaces the
    /// property section, mirroring the Phase 6.1 pass viewer.
    void set_atlas_viewer_visible(bool visible) {
        atlas_viewer_visible_ = visible;
    }
    bool atlas_viewer_visible() const { return atlas_viewer_visible_; }
    void toggle_atlas_viewer() {
        atlas_viewer_visible_ = !atlas_viewer_visible_;
    }

    /// Number of atlas rows the viewer laid out on the most recent
    /// paint() while it was visible. Visible for tests so they can
    /// assert the tab rendered the expected rows without scraping
    /// pixels. Zero when the tab is hidden or no inventory is wired.
    std::size_t atlas_row_count() const { return atlas_row_count_; }

    // ── Phase 3e — 20× zoom loupe ───────────────────────────────────
    //
    // A magnified-pixel preview panel ("loupe") that shows the region
    // under the cursor blown up by `zoom_factor_`. It complements the
    // Phase 3c eyedropper: where the eyedropper grabs a single pixel,
    // the loupe shows a grid of surrounding pixels with a center
    // crosshair so the user can align edges + verify color boundaries.
    //
    // Toggled with the `Z` key (drag=D, eyedropper=E, panel=T are
    // already taken). When active, mouse-move re-centers the sampled
    // region on the cursor; paint() draws paint_zoom_panel() last so
    // the loupe sits on top of everything, including the props panel.
    //
    // Pixel source mirrors the eyedropper's dual path:
    //   - Skia raster surface  → Canvas::read_pixels() gives true RGBA
    //   - non-Skia / headless  → graceful degradation: the loupe still
    //     paints its frame + readout, filling the grid with the
    //     resolved background color of the View under the cursor (or a
    //     neutral checkerboard when that View has no background).
    bool zoom_active() const { return zoom_active_; }
    void set_zoom_active(bool active);
    void toggle_zoom() { set_zoom_active(!zoom_active_); }

    /// Magnification factor — how many panel pixels per source pixel.
    /// Defaults to 20× per the roadmap; clamped to [4, 40] so the grid
    /// neither degenerates into a single cell nor overflows the panel.
    int zoom_factor() const { return zoom_factor_; }
    void set_zoom_factor(int factor);

    /// Last cursor position the loupe sampled around, in root (window)
    /// coordinates. The loupe centers its grid on this pixel.
    Point zoom_sample_center() const { return zoom_sample_center_; }

    /// Resolved color of the center (sample) pixel — what the loupe
    /// readout reports as a hex string. Comes from read_pixels() when
    /// available, else the fallback resolved-view-color path.
    Color zoom_center_color() const { return zoom_center_color_; }

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

    // ── Phase 2 — drift-drawer state ────────────────────────────────
    //
    // drifted_ is the cached drift list from the last refresh_drift().
    // drift_drawer_open_ tracks the expand/collapse state; it flips to
    // true automatically the first time drift is detected (so a stale
    // tweak is never silent), and the user can collapse it after.
    // drift_refreshed_once_ guards the first-paint auto-refresh so the
    // drawer is populated even if the host never calls refresh_drift()
    // explicitly.
    std::vector<TweakStore::DriftedTweak> drifted_;
    bool drift_drawer_open_ = false;
    bool drift_refreshed_once_ = false;
    // Hit-rect of the drift drawer header chevron from the last paint,
    // in root coords — used by handle_mouse_event() to toggle the
    // drawer. width==0 means "not painted this frame".
    Rect drift_header_hit_{};

    // ── Phase 5.2 — reconciliation-tab state ────────────────────────
    //
    // reconcile_tab_visible_ tracks the R-key toggle; off by default so
    // the panel layout is unchanged until the user opts in.
    // reconcile_rows_ caches the rows the tab laid out on the last
    // paint so reconcile_row_count() can report them to tests and the
    // tab scroll stays stable across frames.
    bool reconcile_tab_visible_ = false;
    float reconcile_scroll_y_ = 0.0f;
    std::vector<ReconcileRow> reconcile_rows_;

    // ── Phase 6.2 — texture-atlas-viewer state ──────────────────────
    //
    // atlas_viewer_visible_ tracks the `A`-key toggle; off by default
    // so the panel layout is unchanged until the user opts in.
    // atlas_inventory_ is a non-owning pointer to the render layer's
    // snapshot collection (nullptr → graceful empty state).
    // atlas_scroll_y_ keeps the row list stable across frames;
    // atlas_row_count_ caches how many rows the last paint laid out so
    // atlas_row_count() can report it to tests without scraping pixels.
    bool atlas_viewer_visible_ = false;
    const render::AtlasInventory* atlas_inventory_ = nullptr;
    float atlas_scroll_y_ = 0.0f;
    std::size_t atlas_row_count_ = 0;

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

    // ── Phase 3e — zoom loupe state ─────────────────────────────────
    // Off by default; the inspector behaves identically to the
    // pre-3e build until the user opts in via the Z-key toggle.
    bool zoom_active_ = false;
    int zoom_factor_ = 20;                    ///< panel px per source px
    Point zoom_sample_center_{};              ///< cursor pos last sampled
    Color zoom_center_color_{};               ///< resolved center-pixel color
    bool zoom_center_from_readback_ = false;   ///< true = real read_pixels()

    // Recompute zoom_center_color_ for the current zoom_sample_center_,
    // sampling `canvas` via read_pixels() when the surface supports it
    // and falling back to the resolved View color otherwise. Sets
    // zoom_center_from_readback_ to record which path was taken.
    void update_zoom_sample(Canvas& canvas);
    // Resolve the on-screen color at root-coord point `p` WITHOUT a
    // pixel readback — walks the view tree top-most-first and returns
    // the deepest hit View's background color. Returns false (and
    // leaves `out` untouched) when no background-bearing view is hit.
    bool resolve_view_color_at(Point p, Color& out) const;

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
    /// Phase 2 — paint the drift drawer (collapsible orphaned-tweak
    /// list). Returns the height it consumed so paint_panel can lay out
    /// the props section below it. Paints nothing and returns 0 when
    /// there is no drift.
    float paint_drift_drawer(Canvas& canvas, float x, float y, float w);
    // Phase 3e — magnified-pixel loupe. Draws a fixed-corner panel with
    // a grid of `zoom_factor_`-scaled pixels sampled around the cursor,
    // a center crosshair marking the sample pixel, and a coordinate +
    // hex color readout strip beneath the grid.
    void paint_zoom_panel(Canvas& canvas);

    // Phase 2.5 — render the tweak management panel into the given
    // rect. Repopulates tweak_rows_ with this frame's hit-rects.
    void paint_tweaks_section(Canvas& canvas, float x, float y, float w, float h);

    /// Phase 5.2 — render the reconciliation tab into the panel region
    /// normally occupied by the property section. Each stored tweak
    /// gets a row showing its anchor, property path, and a color-coded
    /// reconciliation-status badge (locked / drift / unresolvable).
    /// Repopulates reconcile_rows_ with this frame's classified rows.
    void paint_reconcile_tab(Canvas& canvas, float x, float y, float w, float h);

    /// Phase 6.2 — render the texture-atlas viewer into the panel
    /// region normally occupied by the property section. Each
    /// registered atlas gets a row showing its kind label, pixel
    /// dimensions, page count, live entry count, and an occupancy bar.
    /// Updates atlas_row_count_ with the number of rows laid out.
    void paint_atlas_tab(Canvas& canvas, float x, float y, float w, float h);

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

    // Phase 3e — zoom loupe layout. The loupe samples a square grid of
    // kZoomGridCells × kZoomGridCells source pixels (odd so there's an
    // exact center pixel) and renders each at zoom_factor_ scale. The
    // panel sits in a fixed corner with a readout strip beneath.
    static constexpr int   kZoomGridCells   = 11;   ///< odd → exact center
    static constexpr int   kZoomFactorMin   = 4;
    static constexpr int   kZoomFactorMax   = 40;
    static constexpr float kZoomReadoutH    = 36.0f;
    static constexpr float kZoomPanelMargin = 12.0f;
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
