// inspector_overlay.cpp — Visual inspector overlay implementation
//
// Per-feature overlay clusters live in sibling TUs (roadmap P10-2):
//   inspector_overlay_field_edit.cpp   — Phase 3b live-editable fields
//   inspector_overlay_zoom.cpp         — Phase 3e 20× zoom loupe
//   inspector_overlay_pass_viewer.cpp  — Phase 6.1 pass-attribution viewer
// Shared overlay color constants are declared in
// inspector_overlay_internal.hpp.

#include "inspector_overlay_internal.hpp"

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/render/render_pass.hpp>
#include <pulp/render/atlas_inventory.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace pulp::inspect {

// Format a Color as a CSS hex string: "#rrggbb" when fully opaque,
// "#rrggbbaa" otherwise. Lower-case, fixed-width — matches the hex
// shape the JS bridge and pulp-tweaks.json already use for colors.

// ── Constructor ─────────────────────────────────────────────────────────────

InspectorOverlay::InspectorOverlay(View& root) : root_(root) {}

void install_inspector_hooks(InspectorOverlay& inspector) {
    g_active_inspector = &inspector;
    // Install all hooks via function pointers — no circular dependency
    View::set_inspector_paint_hook([&inspector](Canvas& canvas) {
        inspector.paint(canvas);
    });
    View::set_inspector_key_hook([&inspector](const KeyEvent& e) -> bool {
        return inspector.handle_key_event(e);
    });
    View::set_inspector_mouse_hook([&inspector](const MouseEvent& e) -> bool {
        return inspector.handle_mouse_event(e);
    });
}

void InspectorOverlay::set_active(bool active) {
    active_ = active;
    if (active) {
        // Re-check drift each time the inspector opens — the design may
        // have been re-imported while the overlay was dismissed.
        drift_refreshed_once_ = false;
    }
    if (!active) {
        // Dropping selection while editing would leave a dangling
        // edit_target_view_ — cancel first so the buffer state is
        // cleared before we null out the target.
        if (!editing_field_.empty()) cancel_field_edit();
        selected_ = nullptr;
        hovered_ = nullptr;
        alt_hover_target_ = nullptr;
        distance_anchor_ = nullptr;
        editable_fields_.clear();
        // Phase 3c — drop any pending eyedropper state with the
        // inspector so a stale swatch never paints on the next open.
        eyedropper_active_ = false;
        eyedropper_has_sample_ = false;
        // Phase 3e — the loupe is a transient inspect tool; dismissing
        // the whole inspector also closes it so re-opening starts clean.
        zoom_active_ = false;
        // Phase 5.2 — drop the reconciliation tab's laid-out rows so
        // reconcile_row_count() reports 0 while the inspector is shut.
        // The R-key toggle state itself is left intact (mirrors how
        // the tweaks panel keeps tweaks_panel_visible_ across opens).
        reconcile_rows_.clear();
        // Phase 6.2 — reset the atlas viewer's laid-out row count so
        // atlas_row_count() reports 0 while the inspector is shut. As
        // with the reconciliation tab the `A`-key toggle state is left
        // intact so re-opening the inspector restores the same tab.
        atlas_row_count_ = 0;
    }
}

// ── Phase 3c — color eyedropper ─────────────────────────────────────────────
//
// An eyedropper mode (E-key, mirroring Phase 3a's D-key for drag
// handles) lets the user sample a color from the rendered UI and
// apply it as a tweak to the selected view's color property.
//
// Two sampling paths, picked at runtime:
//   1. Framebuffer readback — `Canvas::read_pixels()` returns the
//      exact rendered pixel under the cursor. Implemented only on
//      Skia raster surfaces (see canvas.hpp issue-916); when present
//      it is authoritative because it captures gradients, borders,
//      child paint, and theme blending the View tree alone can't.
//   2. Resolved-style fallback — when readback is unavailable
//      (RecordingCanvas, CG fallback, headless tests), the
//      eyedropper samples the resolved background color of the
//      top-most View under the cursor, walking up to the nearest
//      ancestor with an explicit background. This is a documented v1
//      simplification: it sees declared View backgrounds, not
//      arbitrary pixels.
//
// On click the sampled color is emitted via emit_tweak_for_selection()
// — the SAME path Phase 3a/3b gestures use — encoded as a "#rrggbb"
// hex string, source "inspector-eyedropper".

void InspectorOverlay::set_eyedropper_active(bool active) {
    eyedropper_active_ = active;
    if (!active) eyedropper_has_sample_ = false;
}

bool InspectorOverlay::resolved_color_under(Point pos, Color& out) const {
    const View* hit = root_.hit_test(pos);
    for (const View* v = hit; v; v = v->parent()) {
        if (v->has_background_color()) {
            out = v->background_color();
            return true;
        }
    }
    return false;
}

bool InspectorOverlay::sample_color_at(Point pos, Canvas* canvas,
                                       Color& out) const {
    // Path 1 — framebuffer readback (Skia raster only). read_pixels
    // returns RGBA8 in `px`; a false return (RecordingCanvas / CG
    // fallback) drops through to the resolved-style path.
    if (canvas) {
        std::uint8_t px[4] = {0, 0, 0, 0};
        int ix = static_cast<int>(std::lround(pos.x));
        int iy = static_cast<int>(std::lround(pos.y));
        if (ix >= 0 && iy >= 0 &&
            canvas->read_pixels(ix, iy, 1, 1, px)) {
            out = Color::rgba8(px[0], px[1], px[2], px[3]);
            return true;
        }
    }
    // Path 2 — resolved-style fallback.
    return resolved_color_under(pos, out);
}

bool InspectorOverlay::apply_eyedropper_pick() {
    if (!eyedropper_has_sample_) return false;
    bool ok = emit_tweak_for_selection(
        eyedropper_target_,
        choc::value::createString(color_to_hex(eyedropper_sample_)),
        "inspector-eyedropper");
    // A pick is a single deliberate action — disable the mode so the
    // very next click resumes normal selection rather than picking
    // again. (Re-arm with the E key for another pick.) We disable
    // even on a no-op emit so the UX is predictable: the user clicked
    // with the eyedropper, the eyedropper is now spent.
    set_eyedropper_active(false);
    return ok;
}

// ── Phase 0b PR-C-1: in-process gesture-tweak emission ─────────────────────
//
// Routes overlay-driven direct-manipulation edits (drag handles, color
// pick, field edit — Phase 3a builds the actual UI on top of this) to
// the in-process TweakStore. The protocol path (Inspector.applyTweak
// over TCP) still works for remote clients; this is the fast in-process
// path that avoids JSON marshaling for overlay gestures.
//
// Returns false (silent no-op) when any precondition isn't met:
//   - no view currently selected (selected_ == nullptr)
//   - the selected view has no anchor_id (not imported from a design)
//   - no TweakStore wired into the overlay
// All three are valid runtime states (e.g. inspector active on a
// hand-authored UI with no imports). False = "didn't apply"; the caller
// decides whether that's noteworthy.
bool InspectorOverlay::emit_tweak_for_selection(std::string_view property_path,
                                                choc::value::Value value,
                                                std::string_view source) {
    if (!selected_) return false;
    const auto& anchor = selected_->anchor_id();
    if (anchor.empty()) return false;
    if (!tweak_store_) return false;
    tweak_store_->apply_tweak(anchor, property_path, std::move(value), source);
    return true;
}

// ── Phase 2 — drift detection ───────────────────────────────────────────────
//
// Walks the live view tree collecting every non-empty anchor_id, then
// diffs the attached TweakStore against that anchor set. Tweaks whose
// anchor is no longer present are "orphaned" — they silently do nothing
// because direct manipulation can't re-find the element. The drawer
// surfaces them so a design re-import never quietly drops the user's
// edits.

namespace {

void collect_anchor_ids(const View& v, std::vector<std::string>& out) {
    const auto& a = v.anchor_id();
    if (!a.empty()) out.push_back(a);
    for (std::size_t i = 0; i < v.child_count(); ++i)
        collect_anchor_ids(*v.child_at(i), out);
}

}  // namespace

void InspectorOverlay::refresh_drift() {
    drift_refreshed_once_ = true;
    if (!tweak_store_) {
        drifted_.clear();
        return;
    }
    std::vector<std::string> live;
    collect_anchor_ids(root_, live);
    auto next = tweak_store_->find_drifted(live);
    // Auto-expand the drawer the first time drift appears — a stale
    // tweak must never be silent. Once the user collapses it we leave
    // it collapsed even if the count changes.
    if (!next.empty() && drifted_.empty()) {
        drift_drawer_open_ = true;
    }
    drifted_ = std::move(next);
}

// ── Phase 5.2 — reconciliation tab ──────────────────────────────────────────
//
// Classifies every stored tweak into one of three reconciliation
// states (locked-to-source / drifted / unresolvable). The classifier
// is purely a *read* over the existing TweakStore + live view tree —
// it never mutates either, never spawns a process, and never throws.
// It deliberately reuses the drift machinery's "anchor present in the
// live tree" notion (TweakStore::DriftReason::anchor_not_found) so the
// reconciliation tab and the Phase 2 drift drawer agree on what counts
// as orphaned.

const char* InspectorOverlay::reconcile_status_str(ReconcileStatus status) {
    switch (status) {
        case ReconcileStatus::locked_to_source: return "locked-to-source";
        case ReconcileStatus::drifted:          return "drifted";
        case ReconcileStatus::unresolvable:     return "unresolvable";
    }
    return "unknown";
}

InspectorOverlay::ReconcileReport InspectorOverlay::reconcile_report() const {
    ReconcileReport report;
    if (!tweak_store_) return report;

    // Snapshot the live tree's anchor set once — O(tree) — so the
    // per-tweak classification below is an O(1) hash lookup rather
    // than an O(tree) walk per tweak.
    std::vector<std::string> live;
    collect_anchor_ids(root_, live);
    std::unordered_set<std::string> live_anchors(live.begin(), live.end());

    auto records = tweak_store_->list_tweaks();
    // Stable ordering so the tab doesn't reshuffle across frames:
    // group by anchor, anchors sorted, insertion order within an
    // anchor (list_tweaks() preserves it).
    std::stable_sort(records.begin(), records.end(),
                     [](const TweakStore::Record& a, const TweakStore::Record& b) {
                         return a.anchor_id < b.anchor_id;
                     });

    report.rows.reserve(records.size());
    for (const auto& rec : records) {
        ReconcileRow row;
        row.anchor_id = rec.anchor_id;
        row.property_path = rec.property_path;

        const bool resolves = live_anchors.count(rec.anchor_id) != 0;
        if (!resolves) {
            // Conservative fallback — the anchor is gone from the live
            // tree, so the inspector cannot tell where this edit
            // belongs. Locking it is impossible until the anchor is
            // re-established; show it as unresolvable rather than guess.
            row.status = ReconcileStatus::unresolvable;
            ++report.unresolvable_count;
        } else if (tweak_store_->is_locked(rec.anchor_id)) {
            // The anchor resolves AND the user has locked it — the
            // tweak is (or will be) promoted into the authored source,
            // so it survives a fresh re-import. This is "reconciled".
            row.status = ReconcileStatus::locked_to_source;
            ++report.locked_count;
        } else {
            // The anchor resolves but is unlocked — the edit lives
            // only in the runtime tweak layer. A re-import that
            // regenerates the element would drop it.
            row.status = ReconcileStatus::drifted;
            ++report.drifted_count;
        }
        report.rows.push_back(std::move(row));
    }
    return report;
}

// ── Coordinate helpers ──────────────────────────────────────────────────────

Rect InspectorOverlay::view_bounds_in_root(const View* v) const {
    if (!v) return {};
    float x = 0, y = 0;
    const View* cur = v;
    while (cur && cur != &root_) {
        x += cur->bounds().x;
        y += cur->bounds().y;
        cur = cur->parent();
    }
    return {x, y, v->bounds().width, v->bounds().height};
}

// ── Phase 3a — drag handle hit-test ────────────────────────────────────────
// Each handle is an 8×8 box centered on a corner of the selected view's
// bounds (root coords). We test against a slightly-larger 12×12 grab
// rectangle for forgiving hit detection — corners are small targets,
// and Fitts's law rewards generous hit boxes.
InspectorOverlay::DragCorner
InspectorOverlay::hit_test_drag_handle(Point pos) const {
    if (!selected_) return DragCorner::none;
    if (!dragging_enabled_) return DragCorner::none;
    auto r = view_bounds_in_root(selected_);
    constexpr float kGrab = 6.0f;  // half-side of the 12px grab box
    auto in_box = [&](float cx, float cy) {
        return pos.x >= cx - kGrab && pos.x <= cx + kGrab &&
               pos.y >= cy - kGrab && pos.y <= cy + kGrab;
    };
    if (in_box(r.x,             r.y))              return DragCorner::nw;
    if (in_box(r.x + r.width,   r.y))              return DragCorner::ne;
    if (in_box(r.x,             r.y + r.height))   return DragCorner::sw;
    if (in_box(r.x + r.width,   r.y + r.height))   return DragCorner::se;
    return DragCorner::none;
}

// ── Flat tree ───────────────────────────────────────────────────────────────

void InspectorOverlay::rebuild_flat_tree() {
    flat_tree_.clear();
    std::function<void(const View*, int)> walk = [&](const View* v, int depth) {
        flat_tree_.push_back({v, depth});
        if (collapsed_.count(v)) return;
        for (size_t i = 0; i < v->child_count(); ++i)
            walk(v->child_at(i), depth + 1);
    };
    walk(&root_, 0);

    // Validate selected/hovered still in tree
    auto in_tree = [&](const View* v) {
        if (!v) return true;
        for (auto& item : flat_tree_)
            if (item.view == v) return true;
        return false;
    };
    if (!in_tree(selected_)) selected_ = nullptr;
    if (!in_tree(hovered_)) hovered_ = nullptr;
    if (!in_tree(distance_anchor_)) distance_anchor_ = nullptr;
    if (!in_tree(alt_hover_target_)) alt_hover_target_ = nullptr;
}

// ── Input handling ──────────────────────────────────────────────────────────

bool InspectorOverlay::handle_key_event(const KeyEvent& event) {
    // Cmd+I (macOS) / Ctrl+I (Windows/Linux) toggles inspector
    if (event.key == KeyCode::i && event.isMainModifier() && event.is_down) {
        toggle();
        return true;
    }

    // Phase 3b — field-edit mode owns the keyboard while a numeric
    // value is being edited. Esc cancels, Enter commits, Tab walks
    // to the next field; arrows nudge; digits/sign/decimal extend
    // the buffer. The plain Escape-exits-inspector path below only
    // fires when no edit is in progress (cancel_field_edit() leaves
    // the inspector active so the user can keep poking around).
    if (active_ && !editing_field_.empty() && event.is_down) {
        if (handle_edit_key(event)) return true;
    }

    // Escape exits inspector mode (only when not editing — edit mode
    // already consumed the Esc above to cancel the edit).
    if (active_ && event.key == KeyCode::escape && event.is_down) {
        set_active(false);
        return true;
    }

    // Phase 3a — D toggles drag-handles mode (no modifier; only when
    // the inspector is active so a hotkey collision in a plain text
    // input doesn't accidentally flip drag mode).
    if (active_ && event.key == KeyCode::d && event.is_down &&
        event.modifiers == 0) {
        toggle_dragging();
        return true;
    }

    // Phase 6.1 — P toggles the per-pass attribution viewer (no
    // modifier; only while active, same rationale as the D toggle).
    if (active_ && event.key == KeyCode::p && event.is_down &&
        event.modifiers == 0) {
        toggle_pass_viewer();
        return true;
    }

    // Phase 2.5 — T toggles the tweak management panel (no modifier;
    // only while the inspector is active). Guarded behind not-editing
    // so typing a 't' into a field-edit buffer doesn't flip the panel.
    if (active_ && editing_field_.empty() && event.key == KeyCode::t &&
        event.is_down && event.modifiers == 0) {
        toggle_tweaks_panel();
        return true;
    }

    // Phase 5.1 — J jumps to the selected view's authored JSX source
    // (no modifier; inspector-active only — same collision-avoidance
    // discipline as the D drag toggle). Graceful no-op when there is
    // no selection or the selection has no source provenance.
    if (active_ && event.key == KeyCode::j && event.is_down &&
        event.modifiers == 0) {
        jump_to_selection_source(/*dry_run=*/false);
        // Consume the key regardless — even a no-op jump (no
        // provenance) should not fall through to the view tree.
        return true;
    }

    // Phase 3c — E toggles eyedropper mode (no modifier; same opt-in
    // discipline as the D-key drag toggle above). Entering edit mode
    // already swallows keys before this point, so the E-key can't
    // collide with numeric-field editing.
    if (active_ && event.key == KeyCode::e && event.is_down &&
        event.modifiers == 0) {
        toggle_eyedropper();
        return true;
    }

    // Phase 3e — Z toggles the 20× zoom loupe (no modifier; only when
    // the inspector is active, same guard as the D-key path). D/E/T
    // are already claimed by drag / eyedropper / panel, so Z is the
    // natural free letter for "zoom".
    if (active_ && event.key == KeyCode::z && event.is_down &&
        event.modifiers == 0) {
        toggle_zoom();
        return true;
    }

    // Phase 5.2 — R toggles the reconciliation tab (no modifier; only
    // while the inspector is active, same opt-in discipline as the D /
    // E / P / Z toggles). Guarded behind not-editing so typing an 'r'
    // into a field-edit buffer can't flip the tab. D/E/T/J/P/Z are
    // already claimed, so R ("reconcile") is the natural free letter.
    if (active_ && editing_field_.empty() && event.key == KeyCode::r &&
        event.is_down && event.modifiers == 0) {
        toggle_reconcile_tab();
        return true;
    }

    // Phase 6.2 — A toggles the texture-atlas viewer tab (no modifier;
    // only while the inspector is active, same opt-in discipline as the
    // D / E / P / Z / R toggles). Guarded behind not-editing so typing
    // an 'a' into a field-edit buffer can't flip the tab. D/E/T/J/P/Z/R
    // are already claimed, so A ("atlas") is the natural free letter.
    if (active_ && editing_field_.empty() && event.key == KeyCode::a &&
        event.is_down && event.modifiers == 0) {
        toggle_atlas_viewer();
        return true;
    }

    // Phase 3 — M toggles the selection mode between follows_focus
    // (click-to-select; the default) and follows_mouse (selection
    // tracks the pointer). No modifier; inspector-active only, and
    // guarded behind not-editing so typing an 'm' into a field-edit
    // buffer can't flip the mode. D/E/T/J/P/Z/R/A are already claimed,
    // so M ("mode") is the natural free letter.
    if (active_ && editing_field_.empty() && event.key == KeyCode::m &&
        event.is_down && event.modifiers == 0) {
        toggle_selection_mode();
        return true;
    }

    return false;
}

SourceJumpResult InspectorOverlay::jump_to_selection_source(bool dry_run) {
    // selected_ may be null (no selection) or carry no source_loc
    // (view authored outside the JSX-import path). jump_to_source()
    // handles both as a structured ok==false result — no throw, no
    // process spawn — so the caller (J hotkey / protocol) can branch
    // cleanly.
    return jump_to_source(config_, selected_, dry_run);
}

bool InspectorOverlay::handle_mouse_event(const MouseEvent& event) {
    if (!active_) return false;

    auto pos = event.position;

    // Phase 3e — the loupe re-centers on the cursor for EVERY mouse
    // event (move, press, release) while it's active. We only record
    // the position here; the actual pixel sample needs the live Canvas
    // and so happens in paint_zoom_panel(). Recording it for all event
    // kinds — not just moves — keeps the loupe glued to the cursor
    // even during a drag-resize gesture. We do NOT consume the event:
    // the loupe is a passive overlay and other handlers below still
    // need to see the move/press.
    if (zoom_active_) {
        zoom_sample_center_ = pos;
    }

    // ── Phase 3c: eyedropper mode ──────────────────────────────────
    // When the eyedropper is armed it owns canvas-area mouse events:
    // moves sample the color under the cursor (driving the swatch),
    // a press applies the pick. It deliberately yields to the panel
    // (so the user can still click the tree / fields) — a press over
    // the panel falls through to the normal panel path below.
    //
    // The eyedropper's paint() captures the pixel under the cursor at
    // sample time, BEFORE the swatch chrome is drawn, so the chrome
    // never contaminates a subsequent read. Sampling here in the
    // handler instead would read the previous frame; we therefore
    // record the cursor position and let paint() do the readback.
    if (eyedropper_active_ && !point_in_panel(pos)) {
        eyedropper_cursor_ = pos;
        if (event.is_down) {
            // Resolved-style sampling is synchronous + frame-
            // independent, so a click without a prior move still
            // picks a real color (covers headless / scripted use).
            //
            // Codex P1 (#2434): the click must be authoritative on the
            // click coordinate. Invalidate any prior sample FIRST — a
            // hover move or a paint_eyedropper_cursor() framebuffer
            // readback at the default/old cursor position may have left
            // a stale `eyedropper_sample_` with `eyedropper_has_sample_`
            // still true. If the click-position resample then fails
            // (e.g. the click lands where no view carries a background
            // color and only the resolved-style fallback is available),
            // apply_eyedropper_pick() must no-op rather than commit the
            // stale color — so the invalidation has to happen before the
            // resample, not be skipped on a failed read.
            eyedropper_has_sample_ = false;
            Color sampled;
            if (sample_color_at(pos, nullptr, sampled)) {
                eyedropper_sample_ = sampled;
                eyedropper_has_sample_ = true;
            }
            apply_eyedropper_pick();
            return true;  // consume the pick click
        }
        // Move: resolved-style sample now for an immediate swatch;
        // paint() upgrades to framebuffer readback when available.
        Color sampled;
        if (sample_color_at(pos, nullptr, sampled)) {
            eyedropper_sample_ = sampled;
            eyedropper_has_sample_ = true;
        }
        return false;  // don't consume moves — let hover effects run
    }

    // ── Phase 3a: drag-handle gesture state machine ────────────────
    // The Pulp MouseEvent model uses is_down=true ONLY for the
    // initial press; subsequent moves AND the release both arrive
    // as is_down=false (JUCE convention). Without a distinct
    // release flag we adopt: down on a handle starts the drag,
    // every is_down=false event live-resizes + overwrites the
    // tweak, and the NEXT is_down=true event ends the drag (acts
    // as the release). apply_tweak() overwrites the same key so
    // the final tweak value matches the cursor position at
    // release time.
    //
    // Runs BEFORE the panel-area test so a drag started over the
    // canvas is owned by this branch even if the cursor briefly
    // enters the panel mid-drag.
    if (active_drag_ != DragCorner::none && selected_) {
        if (event.is_down) {
            // Release: end the drag. Don't consume — let this click
            // fall through to normal selection logic so the user can
            // immediately re-target without a wasted click.
            active_drag_ = DragCorner::none;
            // fall through to the normal handlers below
        } else {
            // Move: live-resize + overwrite the tweak.
            float dx = pos.x - drag_start_pos_.x;
            float dy = pos.y - drag_start_pos_.y;

            float new_w = drag_start_bounds_.width;
            float new_h = drag_start_bounds_.height;
            switch (active_drag_) {
                case DragCorner::nw: new_w -= dx; new_h -= dy; break;
                case DragCorner::ne: new_w += dx; new_h -= dy; break;
                case DragCorner::sw: new_w -= dx; new_h += dy; break;
                case DragCorner::se: new_w += dx; new_h += dy; break;
                case DragCorner::none: break;
            }
            // Floor at 4px so the view never collapses small enough
            // that handles overlap and the user can't grab them.
            new_w = std::max(4.0f, new_w);
            new_h = std::max(4.0f, new_h);

            // Mutate Yoga inputs (NOT View::set_bounds — Yoga
            // overwrites resolved bounds on next layout pass).
            // preferred_* are the input fields Yoga reads;
            // dim_* keeps the px-unit metadata.
            auto& f = selected_->flex();
            f.preferred_width = new_w;
            f.preferred_height = new_h;
            f.dim_width = {new_w, DimensionUnit::px};
            f.dim_height = {new_h, DimensionUnit::px};
            // Update bounds locally so paint_highlight + hit-test
            // see the new size before the next layout pass.
            auto b = selected_->bounds();
            b.width = new_w;
            b.height = new_h;
            selected_->set_bounds(b);

            // Emit tweaks every tick — apply_tweak() overwrites,
            // so the final value matches release time.
            emit_tweak_for_selection(
                "layout.width",
                choc::value::createFloat32(new_w),
                "inspector-drag-handle");
            emit_tweak_for_selection(
                "layout.height",
                choc::value::createFloat32(new_h),
                "inspector-drag-handle");
            return true;  // consume the move event
        }
    }

    // Phase 3a: hand-off from selection to drag — if drag-handles
    // mode is enabled, a view is selected, and the press lands on a
    // drag handle, START the drag and consume.
    if (event.is_down && active_drag_ == DragCorner::none && selected_) {
        auto handle = hit_test_drag_handle(pos);
        if (handle != DragCorner::none) {
            active_drag_ = handle;
            drag_start_pos_ = pos;
            drag_start_bounds_ = view_bounds_in_root(selected_);
            drag_start_pref_w_ = selected_->flex().preferred_width;
            drag_start_pref_h_ = selected_->flex().preferred_height;
            return true;  // consume the press; subsequent moves are ours
        }
    }

    // Check if mouse is in the panel area
    if (point_in_panel(pos)) {
        // Codex P2 follow-up on #2328: clear Alt-hover state before
        // panel-entry early-return. Without this, moving from an
        // Alt-hovered view straight into the inspector panel leaves
        // `alt_hover_target_` pointing at the previous view and the
        // overlay keeps drawing the live distance line even though
        // the cursor has left the view area.
        alt_hover_target_ = nullptr;

        if (event.is_down) {
            // Phase 2.5 — clicks on a tweak-row icon (bypass / lock /
            // delete) in the management panel. Checked first so an
            // icon click never falls through to tree selection or
            // field-edit. The hit list (tweak_rows_) is populated by
            // the most recent paint_tweaks_section() call.
            if (tweaks_panel_visible_ && tweak_store_) {
                std::size_t row_idx = 0;
                auto action = tweak_action_at(pos, row_idx);
                if (action != TweakAction::none) {
                    const auto& row = tweak_rows_[row_idx];
                    switch (action) {
                        case TweakAction::bypass: {
                            // Toggle whole-anchor bypass. A path-list
                            // bypass collapses to whole-anchor here —
                            // the panel's bypass control is anchor-
                            // scoped (the row just surfaces it).
                            bool now =
                                tweak_store_->is_bypassed(row.anchor_id,
                                                          row.property_path);
                            tweak_store_->set_bypass(row.anchor_id, !now);
                            break;
                        }
                        case TweakAction::lock: {
                            bool now = tweak_store_->is_locked(row.anchor_id);
                            tweak_store_->set_locked(row.anchor_id, !now);
                            break;
                        }
                        case TweakAction::remove:
                            tweak_store_->remove_tweak(row.anchor_id,
                                                       row.property_path);
                            break;
                        case TweakAction::none:
                            break;
                    }
                    return true;
                }
            }

            // Phase 2 — a click on the drift-drawer header toggles the
            // drawer expand/collapse. Checked first because the header
            // overlaps the props-section coordinate range; without
            // this the click would fall through to tree selection.
            if (drift_header_hit_.width > 0 &&
                pos.x >= drift_header_hit_.x &&
                pos.x <= drift_header_hit_.x + drift_header_hit_.width &&
                pos.y >= drift_header_hit_.y &&
                pos.y <= drift_header_hit_.y + drift_header_hit_.height) {
                toggle_drift_drawer();
                return true;
            }

            // Phase 3b — clicks on numeric values in the property
            // panel enter edit mode. The hit list is populated by the
            // most recent paint_props_section() call; we check it
            // BEFORE falling through to the tree-selection path so a
            // click on, say, the "padding" value doesn't also walk
            // the tree row underneath.
            int field_idx = editable_field_at(pos);
            if (field_idx >= 0) {
                const auto& f = editable_fields_[field_idx];
                // Commit any in-progress edit on a different field
                // before switching — same semantics as Tab.
                if (!editing_field_.empty() && editing_field_ != f.path) {
                    commit_field_edit();
                }
                if (editing_field_ != f.path) {
                    begin_field_edit(f.path, f.value);
                }
                return true;
            }

            // Click landed on the panel but not on an editable field:
            // implicitly commit any open edit so the user can move on.
            if (!editing_field_.empty()) {
                commit_field_edit();
            }

            // Click in tree area — select the view
            float panel_x = root_.bounds().width - panel_width_;
            float relative_y = pos.y - tree_scroll_y_;
            auto* item = tree_item_at_y(relative_y);
            if (item) {
                if (pos.x < panel_x + item->depth * kIndent + 16.0f && item->view->child_count() > 0) {
                    // Clicked on collapse toggle
                    if (collapsed_.count(item->view))
                        collapsed_.erase(item->view);
                    else
                        collapsed_.insert(item->view);
                } else {
                    selected_ = const_cast<View*>(item->view);
                }
            }
        }
        return true;  // consume all panel events
    }

    // Clicking outside the panel while editing commits the open edit
    // (matches the blur-to-commit convention of the spec). We do NOT
    // consume the click — the user is presumably selecting a different
    // view in the canvas, which should proceed normally.
    if (event.is_down && !editing_field_.empty()) {
        commit_field_edit();
    }

    // Mouse in view area — pick view under cursor for highlighting
    auto* hit = root_.hit_test(pos);
    if (hit) {
        hovered_ = hit;

        // Phase 3 — selection-mode toggle. In follows_mouse mode the
        // selection chases the pointer: every pointer-move re-selects
        // the hovered View (Figma-style "select on hover"). In the
        // default follows_focus mode the selection stays pinned and is
        // only changed by an explicit click (handled below). The Alt
        // modifier is excluded so Alt-hover sibling-distance and
        // Alt+click distance-anchor modes keep their pinned selection.
        //
        // A field edit in progress also pins the selection: begin_field_edit()
        // snapshots the edit target, but write_field_value() /
        // commit_field_edit() still operate on the *current* selected_. If a
        // mid-edit hover were allowed to move selected_, the edit would commit
        // to the wrong node (or a no-longer-valid target). follows_focus mode
        // is already safe here because it never chases the pointer.
        if (selection_mode_ == SelectionMode::follows_mouse &&
            !event.is_down && !event.isAltDown() && !is_editing()) {
            selected_ = hit;
        }
    }

    // Phase 3f — Alt-hover sibling distance (Figma-style). Tracks the
    // hovered View as an alt_hover_target_ whenever Alt is held AND a
    // selected_ exists; clears as soon as Alt is released. The dynamic
    // line paints from selected_ to alt_hover_target_ in
    // paint_distance_lines().
    if (event.isAltDown() && selected_ && hit && hit != selected_) {
        alt_hover_target_ = hit;
    } else {
        alt_hover_target_ = nullptr;
    }

    if (event.is_down) {
        // Click: select the hovered view (consume click to prevent widget interaction)
        if (hit) {
            if (event.modifiers & kModAlt) {
                // Alt+click: distance measurement
                if (!distance_anchor_) {
                    distance_anchor_ = hit;
                } else {
                    selected_ = hit;
                }
            } else {
                selected_ = hit;
                distance_anchor_ = nullptr;
            }
        }
        return true;  // consume clicks when inspector is active
    }

    // Hover events: don't consume — let normal hover effects work
    return false;
}

bool InspectorOverlay::point_in_panel(Point p) const {
    float panel_x = root_.bounds().width - panel_width_;
    return p.x >= panel_x;
}

const InspectorOverlay::TreeItem* InspectorOverlay::tree_item_at_y(float y) const {
    int index = static_cast<int>(y / kRowHeight);
    if (index >= 0 && index < static_cast<int>(flat_tree_.size()))
        return &flat_tree_[index];
    return nullptr;
}

// ── Painting ────────────────────────────────────────────────────────────────


// Phase 3b — the live-editable box-model fields (editable_field_at /
// read_field_value / write_field_value / begin_field_edit /
// commit_field_edit / cancel_field_edit / apply_edit_buffer_to_view /
// handle_edit_key) are defined in inspector_overlay_field_edit.cpp.

// Phase 3e — the 20× zoom loupe (set_zoom_active / set_zoom_factor /
// resolve_view_color_at / update_zoom_sample / paint_zoom_panel) is
// defined in inspector_overlay_zoom.cpp.

} // namespace pulp::inspect
