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
#include <pulp/runtime/log.hpp>

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
static std::string color_to_hex(const Color& c) {
    std::ostringstream oss;
    oss << '#' << std::hex << std::nouppercase << std::setfill('0');
    oss << std::setw(2) << static_cast<int>(c.r8())
        << std::setw(2) << static_cast<int>(c.g8())
        << std::setw(2) << static_cast<int>(c.b8());
    if (c.a8() != 255)
        oss << std::setw(2) << static_cast<int>(c.a8());
    return oss.str();
}

// ── Constructor ─────────────────────────────────────────────────────────────

InspectorOverlay::InspectorOverlay(View& root) : root_(root) {}

// P2 two-IR-worlds shim: map a `layout.{position,left,top}` move tweak
// (or the bare leaf) onto live View setters so the move round-trips on
// the C++/native runtime apply path, not just the TS/React path.
bool apply_move_tweak_to_view(View& view,
                              std::string_view property_path,
                              const choc::value::Value& value) {
    // Strip a recognized leading namespace ("layout." / "style.") to
    // the bare leaf; the TS world emits under `layout.*`.
    std::string_view leaf = property_path;
    if (auto dot = property_path.find('.'); dot != std::string_view::npos) {
        std::string_view head = property_path.substr(0, dot);
        if (head == "layout" || head == "style" || head == "paint") {
            leaf = property_path.substr(dot + 1);
        }
    }

    auto to_float = [&](float fallback) -> float {
        if (value.isString()) {
            try { return std::stof(std::string(value.getString())); }
            catch (...) { return fallback; }
        }
        // Numeric: getWithDefault coerces int/float values to double.
        return static_cast<float>(value.getWithDefault<double>(
            static_cast<double>(fallback)));
    };

    if (leaf == "position") {
        std::string s = value.isString() ? std::string(value.getString()) : "";
        if (s == "absolute") view.set_position(View::Position::absolute);
        else if (s == "fixed") view.set_position(View::Position::fixed);
        else if (s == "relative") view.set_position(View::Position::relative);
        else if (s == "static" || s == "static_")
            view.set_position(View::Position::static_);
        else if (s == "sticky") view.set_position(View::Position::sticky);
        else return false;
        return true;
    }
    if (leaf == "left")  { view.set_left(to_float(view.left()));   return true; }
    if (leaf == "top")   { view.set_top(to_float(view.top()));     return true; }
    if (leaf == "right") { view.set_right(to_float(view.right())); return true; }
    if (leaf == "bottom"){ view.set_bottom(to_float(view.bottom()));return true; }
    return false;
}

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

// ── P2a — undo safety net helpers ───────────────────────────────────────────
//
// planning/2026-05-21-wysiwyg-direct-manipulation-extension.md § R2.2.
// These capture the pre-gesture state at gesture START and restore it at
// undo time. A gesture's EditHistory entry pairs:
//   do_fn   = re-apply the after-state (idempotent — the drag already
//             mutated the live view, so EditHistory::perform calling do_fn
//             once more just re-sets the same final values).
//   undo_fn = restore the captured before-state (View inputs + tweaks).

InspectorOverlay::LayoutSnapshot
InspectorOverlay::snapshot_layout(const View* v) const {
    LayoutSnapshot s;
    if (!v) return s;
    const auto& f = v->flex();
    s.preferred_width = f.preferred_width;
    s.preferred_height = f.preferred_height;
    s.dim_width = f.dim_width;
    s.dim_height = f.dim_height;
    s.position = v->position();
    s.left = v->left();
    s.top = v->top();
    s.left_unit = v->left_unit();
    s.top_unit = v->top_unit();
    s.has_left = v->has_left();
    s.has_top = v->has_top();
    s.scale = v->scale();
    s.bounds = v->bounds();
    return s;
}

void InspectorOverlay::restore_layout(View* v, const LayoutSnapshot& s) const {
    if (!v) return;
    auto& f = v->flex();
    f.preferred_width = s.preferred_width;
    f.preferred_height = s.preferred_height;
    f.dim_width = s.dim_width;
    f.dim_height = s.dim_height;
    // Position + insets. There is no public API to clear has_left_ /
    // has_top_ back to false, so we restore the captured raw value + unit
    // unconditionally. For a node whose pre-move position was static_ /
    // relative, Yoga ignores absolute insets for static and applies the
    // restored (typically 0) inset for relative — visually identical to
    // the no-inset original. The position enum is restored exactly.
    v->set_position(s.position);
    v->set_left(s.left, s.left_unit);
    v->set_top(s.top, s.top_unit);
    // P2c — restore proportional-resize content scale.
    v->set_scale(s.scale);
    v->set_bounds(s.bounds);
    v->invalidate_layout();
}

std::vector<InspectorOverlay::PriorTweak>
InspectorOverlay::snapshot_tweaks(
    std::string_view anchor,
    const std::vector<std::string>& paths) const {
    std::vector<PriorTweak> out;
    out.reserve(paths.size());
    for (const auto& p : paths) {
        PriorTweak pt;
        pt.path = p;
        if (tweak_store_)
            pt.value = tweak_store_->lookup(anchor, p);
        out.push_back(std::move(pt));
    }
    return out;
}

void InspectorOverlay::restore_tweaks(std::string_view anchor,
                                      const std::vector<PriorTweak>& prior,
                                      std::string_view source) const {
    if (!tweak_store_) return;
    for (const auto& pt : prior) {
        if (pt.value.has_value())
            tweak_store_->apply_tweak(anchor, pt.path, *pt.value, source);
        else
            tweak_store_->remove_tweak(anchor, pt.path);
    }
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

// ── P2 — drag-to-move helpers ───────────────────────────────────────────────

bool InspectorOverlay::hit_test_body(Point pos) const {
    if (!selected_) return false;
    if (!dragging_enabled_) return false;
    // A handle press always wins over a body press, so a point on a
    // handle is NOT a body hit.
    if (hit_test_drag_handle(pos) != DragCorner::none) return false;
    auto r = view_bounds_in_root(selected_);
    return pos.x >= r.x && pos.x <= r.x + r.width &&
           pos.y >= r.y && pos.y <= r.y + r.height;
}

bool InspectorOverlay::selected_parent_is_grid() const {
    if (!selected_ || !selected_->parent()) return false;
    return selected_->parent()->layout_mode() == LayoutMode::grid;
}

const View* InspectorOverlay::containing_block_of(const View* v,
                                                  Rect& block_root_out) const {
    // Walk up to the nearest ancestor Yoga treats as a containing block:
    // any View whose position is relative / absolute / fixed / sticky.
    // (Pulp maps static_ -> Yoga Relative, so a static_ ancestor also
    // forms a containing block in practice — but we prefer an explicitly
    // non-static ancestor and otherwise fall back to the root, which
    // always forms one.)
    const View* cur = v ? v->parent() : nullptr;
    while (cur && cur != &root_) {
        if (cur->position() != View::Position::static_) {
            block_root_out = view_bounds_in_root(cur);
            return cur;
        }
        cur = cur->parent();
    }
    block_root_out = view_bounds_in_root(&root_);
    return &root_;
}

void InspectorOverlay::seed_move_origin(const View* v) {
    if (!v) { move_seed_left_ = move_seed_top_ = 0.0f; return; }
    Rect block_root{};
    const View* block = containing_block_of(v, block_root);
    (void)block;
    const Rect child_root = view_bounds_in_root(v);

    // Resolved per-edge border of the containing block (Yoga subtracts
    // border + inset + child-margin to position a defined inset; see
    // AbsoluteLayout.cpp:209-224 and the plan's border-edge formula).
    float block_border_left = 0.0f;
    float block_border_top = 0.0f;
    if (block) {
        if (block->has_border_left_set())
            block_border_left = std::max(0.0f, block->border_left_width());
        else if (block->has_border())
            block_border_left = std::max(0.0f, block->border_width());
        if (block->has_border_top_set())
            block_border_top = std::max(0.0f, block->border_top_width());
        else if (block->has_border())
            block_border_top = std::max(0.0f, block->border_width());
    }
    const float child_margin_left = v->flex().margin_l();
    const float child_margin_top = v->flex().margin_t();

    // left = childRootX - blockRootX - blockBorderLeft - childMarginLeft
    move_seed_left_ = child_root.x - block_root.x - block_border_left
                    - child_margin_left;
    move_seed_top_ = child_root.y - block_root.y - block_border_top
                   - child_margin_top;
}

// ── P2c — reflow-aware move (drop-target resolution + commit) ───────────────

bool InspectorOverlay::is_self_or_ancestor(const View* ancestor,
                                           const View* v) {
    for (const View* cur = v; cur; cur = cur->parent())
        if (cur == ancestor) return true;
    return false;
}

void InspectorOverlay::reparent_view(View* v, View* new_parent, int index) {
    if (!v || !new_parent) return;
    View* old_parent = v->parent();
    if (old_parent == new_parent) {
        // Same-parent reorder is expressed via flex().order by the caller;
        // the children_ vector index isn't load-bearing for visual order
        // (layout sorts by order). Nothing structural to do here.
        return;
    }
    std::unique_ptr<View> owned;
    if (old_parent) {
        owned = old_parent->remove_child(v);
    }
    if (!owned) {
        // v had no parent (or removal failed) — can't reparent safely.
        return;
    }
    new_parent->add_child(std::move(owned));
    // add_child appends; visual position within new_parent is controlled by
    // flex().order, which the caller sets. `index` is advisory for future
    // exact-index insertion (View has no insert-at-index API today).
    (void)index;
    if (old_parent) old_parent->invalidate_layout();
    new_parent->invalidate_layout();
}

void InspectorOverlay::resolve_drop_target(Point pos) {
    drop_target_ = nullptr;
    drop_index_ = 0;
    drop_inside_ = false;
    drop_indicator_ = {};
    drop_indicator_is_line_ = false;
    if (!selected_) return;

    // Find the deepest flex CONTAINER under the cursor that is NOT inside the
    // dragged subtree and is NOT a grid. A "container" must have at least one
    // child OR be a node the cursor is directly over with room to drop into;
    // a childless LEAF that happens to be under the cursor is treated as a
    // sibling — we drop NEXT TO it (its parent becomes the container), not
    // INTO it. This makes "drag a over c" a reorder within their shared
    // parent, while "drag a into an empty panel" reparents.
    std::function<View*(View*)> deepest_container = [&](View* v) -> View* {
        if (!v) return nullptr;
        const Rect r = view_bounds_in_root(v);
        const bool inside = pos.x >= r.x && pos.x <= r.x + r.width &&
                            pos.y >= r.y && pos.y <= r.y + r.height;
        if (!inside) return nullptr;
        // Recurse into children first for the deepest hit.
        for (size_t i = 0; i < v->child_count(); ++i) {
            if (View* hit = deepest_container(v->child_at(i))) return hit;
        }
        // v itself qualifies as a container only if it's not the dragged
        // subtree and not a grid. A childless leaf qualifies ONLY as a
        // drop-inside target when it isn't a sibling of the dragged node
        // (i.e. dropping into a different empty container). If v is a leaf
        // that shares the dragged node's parent, it's a sibling → don't
        // descend onto it; let the parent be the reorder container.
        if (is_self_or_ancestor(selected_, v)) return nullptr;
        if (v->layout_mode() == LayoutMode::grid) return nullptr;
        const bool is_sibling = (v->parent() == selected_->parent());
        if (v->child_count() == 0 && is_sibling) return nullptr;
        return v;
    };

    View* container = deepest_container(&root_);
    if (!container) return;

    // The dragged element's current parent — used to distinguish a same-
    // parent REORDER from a cross-parent REPARENT (drop INSIDE).
    View* dragged_parent = selected_->parent();
    drop_target_ = container;
    drop_inside_ = (container != dragged_parent);

    // Resolve an insertion index among the container's *visible* children
    // (excluding the dragged element itself) by the cursor's main-axis
    // position. We use the flex main axis: row/row-reverse → x, else y.
    const bool horizontal =
        container->flex().direction == FlexDirection::row ||
        container->flex().direction == FlexDirection::row_reverse;

    struct Slot { View* child; float mid; Rect bounds; };
    std::vector<Slot> slots;
    for (size_t i = 0; i < container->child_count(); ++i) {
        View* c = container->child_at(i);
        if (c == selected_) continue;  // ignore the dragged node
        if (!c->visible()) continue;
        Rect b = view_bounds_in_root(c);
        float mid = horizontal ? (b.x + b.width * 0.5f)
                               : (b.y + b.height * 0.5f);
        slots.push_back({c, mid, b});
    }

    // Index = count of siblings whose midpoint is before the cursor.
    int idx = 0;
    const float cursor_main = horizontal ? pos.x : pos.y;
    for (const auto& s : slots) {
        if (cursor_main > s.mid) idx++;
    }
    drop_index_ = idx;

    // Build the paint affordance:
    //  - REORDER (same parent, has siblings): blue insertion LINE at the gap.
    //  - DROP-INSIDE (different parent) OR empty container: container HIGHLIGHT.
    const Rect cr = view_bounds_in_root(container);
    if (!drop_inside_ && !slots.empty()) {
        // Insertion line between siblings idx-1 and idx.
        drop_indicator_is_line_ = true;
        if (horizontal) {
            float line_x;
            if (idx <= 0) line_x = slots.front().bounds.x;
            else if (idx >= static_cast<int>(slots.size()))
                line_x = slots.back().bounds.x + slots.back().bounds.width;
            else line_x = slots[idx].bounds.x;
            drop_indicator_ = {line_x - 1.0f, cr.y, 2.0f, cr.height};
        } else {
            float line_y;
            if (idx <= 0) line_y = slots.front().bounds.y;
            else if (idx >= static_cast<int>(slots.size()))
                line_y = slots.back().bounds.y + slots.back().bounds.height;
            else line_y = slots[idx].bounds.y;
            drop_indicator_ = {cr.x, line_y - 1.0f, cr.width, 2.0f};
        }
    } else {
        // Container highlight (drop-inside / empty container).
        drop_indicator_is_line_ = false;
        drop_indicator_ = cr;
    }
}

bool InspectorOverlay::commit_reflow_drop(View* dragged) {
    if (!dragged || !drop_target_) return false;
    View* target = drop_target_;
    View* old_parent = dragged->parent();

    // Guard: never drop a node into its own subtree.
    if (is_self_or_ancestor(dragged, target)) return false;

    bool changed = false;
    if (target != old_parent) {
        // REPARENT (drop INSIDE another container) — structural edit.
        reparent_view(dragged, target, drop_index_);
        changed = true;
    }

    // (Re)assign flex().order across the target's children so the dragged
    // node lands at drop_index_. We rebuild a clean 0..N-1 order sequence
    // (normalize) with the dragged node inserted at drop_index_, preserving
    // the relative order of the others.
    std::vector<View*> others;
    for (size_t i = 0; i < target->child_count(); ++i) {
        View* c = target->child_at(i);
        if (c == dragged) continue;
        if (!c->visible()) continue;
        others.push_back(c);
    }
    // stable order: sort others by their current flex().order to preserve
    // the visual sequence the user sees.
    std::stable_sort(others.begin(), others.end(),
        [](View* a, View* b) { return a->flex().order < b->flex().order; });

    int insert_at = std::clamp(drop_index_, 0,
                               static_cast<int>(others.size()));
    int next_order = 0;
    int assigned = 0;
    for (int i = 0; i <= static_cast<int>(others.size()); ++i) {
        if (i == insert_at) {
            int new_order = next_order++;
            if (dragged->flex().order != new_order) {
                dragged->flex().order = new_order;
                changed = true;
            }
            assigned++;
        }
        if (i < static_cast<int>(others.size())) {
            others[i]->flex().order = next_order++;
        }
    }
    (void)assigned;

    target->invalidate_layout();
    if (old_parent && old_parent != target) old_parent->invalidate_layout();
    dragged->invalidate_layout();
    return changed;
}

bool InspectorOverlay::select_parent() {
    if (!selected_) return false;
    View* p = selected_->parent();
    if (!p) return false;
    selected_ = p;
    return true;
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

    // P2a (undo safety net) — Cmd+Z undo / Cmd+Shift+Z (or Cmd+Y) redo,
    // bound only while the inspector is active and an EditHistory is wired.
    // Checked BEFORE the field-edit / toggle paths below so a manipulation
    // gesture is always reversible regardless of what other mode is on.
    // The actual restore mutates View inputs (invalidate_layout) and the
    // TweakStore; the host's continuous paint loop reflects it next frame.
    // Esc-to-ascend and the letter toggles still work because we only
    // consume the Z / Y combos here. Consume even a no-op (empty history)
    // so the keystroke never falls through to the view tree.
    if (active_ && edit_history_ && event.isMainModifier() && event.is_down) {
        if (event.key == KeyCode::z) {
            if (event.isShiftDown())
                edit_history_->redo();   // Cmd+Shift+Z = redo
            else
                edit_history_->undo();   // Cmd+Z = undo
            return true;
        }
        if (event.key == KeyCode::y) {
            edit_history_->redo();       // Cmd+Y = redo (Windows-style)
            return true;
        }
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

    // Escape: P1 drill-down "ascend to parent". When a non-root view is
    // selected, Esc walks the selection UP to its parent so the user can
    // reach a container after click landed on a deeply-nested child
    // (click resolves to the deepest hittable element). Only when there
    // is nothing left to ascend to (no selection, or selection is a
    // direct child of root with no further useful parent) does Esc fall
    // through to exit the inspector. Edit mode already consumed Esc above
    // to cancel the edit, so this never fires mid-edit.
    if (active_ && event.key == KeyCode::escape && event.is_down) {
        // Ascend while there is a parent to climb to (stops at root,
        // which has no parent). Once at root / unselected, Esc exits.
        if (selected_ && selected_->parent()) {
            select_parent();
            return true;  // ascended one level
        }
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

            // P2a: commit the completed resize as ONE undoable unit. The
            // drag already mutated the live view + overwrote the tweaks on
            // each move tick, so the AFTER-state is whatever is live right
            // now — snapshot it for an idempotent do_fn, pair it with the
            // BEFORE-state captured at gesture start for the undo_fn.
            if (edit_history_ && selected_) {
                View* tgt = selected_;
                const std::string anchor = resize_anchor_;
                LayoutSnapshot before = resize_before_layout_;
                std::vector<PriorTweak> before_tweaks = resize_before_tweaks_;
                LayoutSnapshot after = snapshot_layout(tgt);
                std::vector<PriorTweak> after_tweaks =
                    snapshot_tweaks(anchor, {"layout.width", "layout.height",
                                             "transform.scale"});
                auto* self = this;
                edit_history_->perform(
                    [self, tgt, anchor, after, after_tweaks]() {
                        self->restore_layout(tgt, after);
                        self->restore_tweaks(anchor, after_tweaks,
                                             "inspector-drag-handle");
                    },
                    [self, tgt, anchor, before, before_tweaks]() {
                        self->restore_layout(tgt, before);
                        self->restore_tweaks(anchor, before_tweaks,
                                             "inspector-undo");
                    },
                    "resize");
            }
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

            if (resize_proportional_) {
                // P2c — PROPORTIONAL resize (Shift). Instead of stretching
                // the box, SCALE the container's content uniformly so the
                // panel keeps its internal proportions. The scale factor is
                // the box-size ratio along the larger-magnitude axis,
                // anchored at the gesture-start scale. The box itself still
                // grows (preferred_*) so the scaled content has room; the
                // uniform set_scale() keeps children's relative layout. We
                // pick the ratio from whichever axis the start box was
                // larger in, so a corner drag scales coherently.
                const float w0 = std::max(1.0f, drag_start_bounds_.width);
                const float h0 = std::max(1.0f, drag_start_bounds_.height);
                const float ratio_w = new_w / w0;
                const float ratio_h = new_h / h0;
                // Uniform scale: average the two axis ratios so a diagonal
                // drag scales smoothly, then clamp to a sane range.
                float ratio = 0.5f * (ratio_w + ratio_h);
                float new_scale = std::clamp(drag_start_scale_ * ratio,
                                             0.1f, 10.0f);
                selected_->set_scale(new_scale);
                // Grow the box to match so the scaled content isn't clipped.
                auto& f = selected_->flex();
                f.preferred_width = new_w;
                f.preferred_height = new_h;
                f.dim_width = {new_w, DimensionUnit::px};
                f.dim_height = {new_h, DimensionUnit::px};
                auto b = selected_->bounds();
                b.width = new_w;
                b.height = new_h;
                selected_->set_bounds(b);
                selected_->invalidate_layout();

                // Emit the box-size + scale tweaks every tick (overwrite).
                emit_tweak_for_selection(
                    "layout.width", choc::value::createFloat32(new_w),
                    "inspector-drag-handle");
                emit_tweak_for_selection(
                    "layout.height", choc::value::createFloat32(new_h),
                    "inspector-drag-handle");
                emit_tweak_for_selection(
                    "transform.scale", choc::value::createFloat32(new_scale),
                    "inspector-drag-handle");
                return true;
            }

            // Plain box resize: mutate Yoga inputs (NOT View::set_bounds —
            // Yoga overwrites resolved bounds on next layout pass).
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
    if (event.is_down && active_drag_ == DragCorner::none && !move_active_ &&
        selected_) {
        auto handle = hit_test_drag_handle(pos);
        if (handle != DragCorner::none) {
            active_drag_ = handle;
            drag_start_pos_ = pos;
            drag_start_bounds_ = view_bounds_in_root(selected_);
            drag_start_pref_w_ = selected_->flex().preferred_width;
            drag_start_pref_h_ = selected_->flex().preferred_height;
            // P2c — Shift at press chooses PROPORTIONAL resize (scale the
            // container's content) over plain box resize (reflow children).
            resize_proportional_ = event.isShiftDown();
            drag_start_scale_ = selected_->scale();
            // P2a: capture the pre-resize View inputs + prior tweak values
            // so the commit (release) can push ONE undoable EditHistory
            // entry. No-op cost when edit_history_ is null — the captures
            // are cheap and the commit simply skips the perform() call.
            resize_anchor_ = selected_->anchor_id();
            resize_before_layout_ = snapshot_layout(selected_);
            resize_before_tweaks_ =
                snapshot_tweaks(resize_anchor_, {"layout.width",
                                                 "layout.height",
                                                 "transform.scale"});
            return true;  // consume the press; subsequent moves are ours
        }
    }

    // ── P2: drag-to-move gesture state machine ─────────────────────
    // Same press/move/release convention as the resize machine: while a
    // move is active, is_down=false events live-update + overwrite the
    // tweak batch, the next is_down=true ends the gesture. Runs BEFORE
    // the panel-area test so a move started on the canvas stays owned by
    // this branch even if the cursor briefly enters the panel.
    if (move_active_ && selected_) {
        if (event.is_down) {
            // Release: end the move.
            move_active_ = false;

            if (move_float_) {
                // ── ⌘-drag: ABSOLUTE FLOAT (escape hatch) ──────────────
                // The live view + the three move tweaks are already at
                // their final state from the last move tick. Commit ONE
                // undoable unit: AFTER-state do_fn vs BEFORE-state undo_fn.
                if (edit_history_ && selected_) {
                    View* tgt = selected_;
                    const std::string anchor = move_anchor_;
                    LayoutSnapshot before = move_before_layout_;
                    std::vector<PriorTweak> before_tweaks = move_before_tweaks_;
                    LayoutSnapshot after = snapshot_layout(tgt);
                    std::vector<PriorTweak> after_tweaks = snapshot_tweaks(
                        anchor,
                        {"layout.position", "layout.left", "layout.top"});
                    auto* self = this;
                    edit_history_->perform(
                        [self, tgt, anchor, after, after_tweaks]() {
                            self->restore_layout(tgt, after);
                            self->restore_tweaks(anchor, after_tweaks,
                                                 "inspector-drag-move");
                        },
                        [self, tgt, anchor, before, before_tweaks]() {
                            self->restore_layout(tgt, before);
                            self->restore_tweaks(anchor, before_tweaks,
                                                 "inspector-undo");
                        },
                        "move-float");
                }
            } else {
                // ── plain drag: REFLOW-AWARE (reorder / reparent) ──────
                // Commit the resolved drop. This is a STRUCTURAL/order edit
                // (the tree changes or flex().order rewrites), so the undo
                // entry must restore the original parent + child index +
                // order, not a style tweak. We capture the tree state on
                // both sides so undo/redo round-trips.
                View* tgt = selected_;
                View* before_parent = tgt->parent();
                int before_index = -1;
                int before_order = tgt->flex().order;
                if (before_parent) {
                    for (size_t i = 0; i < before_parent->child_count(); ++i)
                        if (before_parent->child_at(i) == tgt) {
                            before_index = static_cast<int>(i);
                            break;
                        }
                }
                bool changed = commit_reflow_drop(tgt);
                if (changed && edit_history_ && before_parent) {
                    View* after_parent = tgt->parent();
                    int after_index = -1;
                    int after_order = tgt->flex().order;
                    if (after_parent) {
                        for (size_t i = 0; i < after_parent->child_count(); ++i)
                            if (after_parent->child_at(i) == tgt) {
                                after_index = static_cast<int>(i);
                                break;
                            }
                    }
                    auto* self = this;
                    // do_fn re-applies the AFTER tree state; undo_fn restores
                    // BEFORE. reparent_to is the structural primitive (no-op
                    // when parent unchanged), order is rewritten directly.
                    edit_history_->perform(
                        [self, tgt, after_parent, after_index, after_order]() {
                            self->reparent_view(tgt, after_parent, after_index);
                            tgt->flex().order = after_order;
                            if (after_parent) after_parent->invalidate_layout();
                            tgt->invalidate_layout();
                        },
                        [self, tgt, before_parent, before_index, before_order]() {
                            self->reparent_view(tgt, before_parent,
                                                before_index);
                            tgt->flex().order = before_order;
                            if (before_parent) before_parent->invalidate_layout();
                            tgt->invalidate_layout();
                        },
                        "move-reflow");
                }
            }
            // Clear drop affordance after a completed gesture.
            drop_target_ = nullptr;
            drop_indicator_ = {};
            // fall through to the normal handlers below
        } else if (move_float_) {
            // ── ⌘-drag move tick: absolute float ──────────────────────
            float dx = pos.x - move_start_pos_.x;
            float dy = pos.y - move_start_pos_.y;
            float new_left = move_seed_left_ + dx;
            float new_top = move_seed_top_ + dy;

            // Mutate Yoga INPUTS (NOT set_bounds — Yoga overwrites
            // resolved bounds next layout pass). Convert to absolute on
            // the first tick (idempotent thereafter).
            selected_->set_position(View::Position::absolute);
            selected_->set_left(new_left);
            selected_->set_top(new_top);
            auto b = selected_->bounds();
            b.x = new_left;
            b.y = new_top;
            selected_->set_bounds(b);
            selected_->invalidate_layout();

            // Emit the three move tweaks as ONE atomic batch (Risk 6):
            // a partial "left/top without position" state shifts a still-
            // relative node, so all three must persist together.
            if (tweak_store_ && !selected_->anchor_id().empty()) {
                std::vector<TweakStore::BatchEntry> batch;
                batch.push_back({"layout.position",
                                 choc::value::createString("absolute")});
                batch.push_back({"layout.left",
                                 choc::value::createFloat32(new_left)});
                batch.push_back({"layout.top",
                                 choc::value::createFloat32(new_top)});
                tweak_store_->apply_tweaks_batch(selected_->anchor_id(),
                                                 std::move(batch),
                                                 "inspector-drag-move");
            }
            return true;  // consume the move event
        } else {
            // ── plain move tick: REFLOW-AWARE drop-target resolution ───
            // We don't mutate the layout while dragging — instead we resolve
            // a drop target and update the paint affordance (insertion line /
            // container highlight). The actual reorder/reparent happens on
            // release (commit_reflow_drop). This keeps the dragged element
            // visually in place and the rest of the tree stable until commit.
            resolve_drop_target(pos);
            return true;  // consume the move event
        }
    }

    // P2: hand-off from selection to MOVE — if dragging mode is enabled,
    // a view is selected, and the press lands on the view's BODY (not a
    // handle), START a move. Guard grid children: a direct child of a
    // grid container ignores position/top/left, so refuse with a clear
    // affordance instead of a silently-broken drag.
    if (event.is_down && active_drag_ == DragCorner::none && !move_active_ &&
        selected_ && hit_test_body(pos)) {
        // P2c — the modifier picks the move MODE:
        //   plain drag  → REFLOW-AWARE (reorder among siblings / reparent)
        //   ⌘-drag      → ABSOLUTE FLOAT (position:absolute + left/top)
        // The grid guard only applies to the float path: a grid child can't
        // take absolute insets. Reflow (reorder/reparent) is fine for a grid
        // child being dragged OUT into a flex container, but reordering a
        // grid cell in place is ignored by layout_grid — so we still refuse a
        // reflow whose drop target is the same grid parent (handled at commit
        // by resolve_drop_target excluding grid containers as drop targets).
        move_float_ = event.isCmdDown();
        if (move_float_ && selected_parent_is_grid()) {
            // Grid guard (float only): refuse. Record the refusal so paint()
            // can surface a clear "can't float grid child" affordance, and
            // log once so a dev sees why the drag did nothing.
            if (!move_refused_grid_) {
                pulp::runtime::log_warn(
                    "inspector: absolute-float refused - selected view's "
                    "parent is a grid container; grid children ignore "
                    "position/top/left (drag without Cmd to reflow it into a "
                    "flex container instead)");
            }
            move_refused_grid_ = true;
            return true;  // consume so the press isn't misread as a select
        }
        move_refused_grid_ = false;
        move_active_ = true;
        move_start_pos_ = pos;
        drop_target_ = nullptr;
        drop_indicator_ = {};
        // P2a: capture pre-move View inputs (position + insets + bounds)
        // and prior values of the three move tweaks BEFORE seed_move_origin
        // / the first move tick converts the node to absolute, so undo can
        // restore the original layout + tweak state exactly. (Only the float
        // path mutates these; the reflow path uses its own parent/index/order
        // capture at release.)
        move_anchor_ = selected_->anchor_id();
        move_before_layout_ = snapshot_layout(selected_);
        move_before_tweaks_ = snapshot_tweaks(
            move_anchor_,
            {"layout.position", "layout.left", "layout.top"});
        if (move_float_) seed_move_origin(selected_);
        return true;  // consume the press; subsequent moves are ours
    }
    move_refused_grid_ = false;

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
                        case TweakAction::remove: {
                            // P2a: capture the value being deleted FIRST so
                            // the undo can re-apply it, then make the delete
                            // ONE undoable unit. Falls back to a plain
                            // remove when no EditHistory is wired.
                            const std::string del_anchor = row.anchor_id;
                            const std::string del_path = row.property_path;
                            auto prior =
                                tweak_store_->lookup(del_anchor, del_path);
                            if (edit_history_ && prior.has_value()) {
                                auto* self = this;
                                choc::value::Value prior_val = *prior;
                                edit_history_->perform(
                                    [self, del_anchor, del_path]() {
                                        if (self->tweak_store_)
                                            self->tweak_store_->remove_tweak(
                                                del_anchor, del_path);
                                    },
                                    [self, del_anchor, del_path, prior_val]() {
                                        if (self->tweak_store_)
                                            self->tweak_store_->apply_tweak(
                                                del_anchor, del_path,
                                                prior_val, "inspector-undo");
                                    },
                                    "delete-tweak");
                            } else {
                                tweak_store_->remove_tweak(del_anchor,
                                                           del_path);
                            }
                            break;
                        }
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
    // P1: in the minimal manipulate layer there is NO dev side-panel, so
    // the whole canvas is live for selection + drag. Reporting "not in
    // panel" everywhere keeps the move/resize gestures owning the canvas.
    if (manipulate_only_) return false;
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

void InspectorOverlay::paint(Canvas& canvas) {
    if (!active_) return;

    // Phase 6.1 — sample the render-pass manager into the rolling
    // attribution history once per frame. Cheap, no-op when no RPM is
    // attached, and de-duplicated against frame_count() so multiple
    // paints of the same frame don't inflate the history.
    capture_pass_frame();

    rebuild_flat_tree();

    // P1 — minimal manipulate layer: paint ONLY the selection box +
    // handles (no dev side-panel, no box-model bands, no distance lines).
    // The floating InspectorWindow is the inspect/tree/props surface; the
    // in-canvas overlay here is the bare manipulation layer.
    if (manipulate_only_) {
        paint_highlight(canvas);
        paint_drop_indicator(canvas);
        return;
    }

    // Phase 2 — populate the drift list on the first paint after the
    // inspector goes active so the drawer is never empty just because
    // the host forgot to call refresh_drift() explicitly.
    if (!drift_refreshed_once_) refresh_drift();
    paint_highlight(canvas);
    paint_drop_indicator(canvas);
    paint_distance_lines(canvas);
    if (selected_) paint_box_model(canvas, selected_);
    paint_panel(canvas);
    // Phase 3c — eyedropper swatch paints above the panel and
    // highlights so the sampled color is never occluded.
    paint_eyedropper_cursor(canvas);
    // Phase 3e — the loupe paints LAST so its magnified grid sits on
    // top of everything (including the props panel and the eyedropper
    // swatch), like a physical loupe resting on the design surface.
    if (zoom_active_) paint_zoom_panel(canvas);
}

void InspectorOverlay::paint_eyedropper_cursor(Canvas& canvas) {
    if (!eyedropper_active_) return;

    // Upgrade the sample to an exact framebuffer pixel if the live
    // Canvas supports readback. Done here (not in the mouse handler)
    // so the read happens against the fully-composited frame, before
    // the swatch chrome below would otherwise contaminate the pixel.
    {
        Color sampled;
        if (sample_color_at(eyedropper_cursor_, &canvas, sampled)) {
            eyedropper_sample_ = sampled;
            eyedropper_has_sample_ = true;
        }
    }
    if (!eyedropper_has_sample_) return;

    // Swatch + hex readout, offset down-right of the cursor so the
    // pointer itself never sits on top of the chrome being read.
    constexpr float kSwatch = 22.0f;   // swatch square side
    constexpr float kPad    = 4.0f;
    constexpr float kRowH   = 16.0f;
    const std::string hex = color_to_hex(eyedropper_sample_);

    canvas.save();
    canvas.set_font("monospace", kFontSize);
    float text_w = canvas.measure_text(hex);
    float box_w = kPad + kSwatch + kPad + text_w + kPad;
    float box_h = kPad + std::max(kSwatch, kRowH) + kPad;
    float bx = eyedropper_cursor_.x + 14.0f;
    float by = eyedropper_cursor_.y + 14.0f;

    // Keep the chrome on-screen near the right / bottom edges.
    float root_w = root_.bounds().width;
    float root_h = root_.bounds().height;
    if (bx + box_w > root_w) bx = eyedropper_cursor_.x - 14.0f - box_w;
    if (by + box_h > root_h) by = eyedropper_cursor_.y - 14.0f - box_h;

    // Chrome background.
    canvas.set_fill_color(kEyedropChromeBg);
    canvas.fill_rounded_rect(bx, by, box_w, box_h, 4.0f);

    // Checkerboard behind the swatch so transparent samples read as
    // transparent rather than as solid black.
    float sx = bx + kPad;
    float sy = by + kPad;
    const Color kCheckA = Color::rgba(0.75f, 0.75f, 0.75f, 1.0f);
    const Color kCheckB = Color::rgba(0.55f, 0.55f, 0.55f, 1.0f);
    constexpr float kCell = kSwatch / 2.0f;
    for (int cy = 0; cy < 2; ++cy)
        for (int cx = 0; cx < 2; ++cx) {
            canvas.set_fill_color(((cx + cy) & 1) ? kCheckB : kCheckA);
            canvas.fill_rect(sx + cx * kCell, sy + cy * kCell, kCell, kCell);
        }

    // Sampled color on top of the checkerboard.
    canvas.set_fill_color(eyedropper_sample_);
    canvas.fill_rect(sx, sy, kSwatch, kSwatch);

    // Swatch border.
    canvas.set_stroke_color(kEyedropBorder);
    canvas.set_line_width(1.0f);
    canvas.stroke_rect(sx, sy, kSwatch, kSwatch);

    // Hex readout.
    canvas.set_fill_color(kEyedropText);
    canvas.fill_text(hex, sx + kSwatch + kPad,
                     by + box_h / 2.0f + 4.0f);

    canvas.restore();
}

void InspectorOverlay::paint_highlight(Canvas& canvas) {
    // Hovered view highlight (blue)
    if (hovered_ && hovered_ != selected_) {
        auto r = view_bounds_in_root(hovered_);
        canvas.set_fill_color(kHighlightFill);
        canvas.fill_rect(r.x, r.y, r.width, r.height);
        canvas.set_stroke_color(kHighlightStroke);
        canvas.set_line_width(1.5f);
        canvas.stroke_rect(r.x, r.y, r.width, r.height);

        // Tooltip
        auto type = ViewInspector::type_name(*hovered_);
        auto label = type + " " + std::to_string(static_cast<int>(r.width))
                   + "×" + std::to_string(static_cast<int>(r.height));
        canvas.set_font("monospace", kFontSize);
        canvas.set_fill_color(kPanelBg);
        float tw = canvas.measure_text(label);
        canvas.fill_rounded_rect(r.x, r.y - 18, tw + 8, 16, 3);
        canvas.set_fill_color(kPanelText);
        canvas.fill_text(label, r.x + 4, r.y - 5);
    }

    // Selected view highlight (orange)
    if (selected_) {
        auto r = view_bounds_in_root(selected_);
        canvas.set_fill_color(kSelectedFill);
        canvas.fill_rect(r.x, r.y, r.width, r.height);
        canvas.set_stroke_color(kSelectedStroke);
        canvas.set_line_width(2.0f);
        canvas.stroke_rect(r.x, r.y, r.width, r.height);

        // Phase 3a — drag handles. Only when dragging mode is on (opt-
        // in via D key). Four 8×8 filled squares at the corners. The
        // actively-dragged corner paints in a brighter shade so the
        // user sees which handle they grabbed even if the cursor
        // moves slightly off the original target.
        if (dragging_enabled_) {
            constexpr float kHandle = 4.0f;  // half-side of 8px box
            auto paint_handle = [&](float cx, float cy, DragCorner which) {
                bool active = (active_drag_ == which);
                canvas.set_fill_color(active
                    ? Color::rgba(1.0f, 0.7f, 0.2f, 1.0f)   // active = bright orange
                    : Color::rgba(1.0f, 0.5f, 0.0f, 0.9f)); // idle = same orange as kSelectedStroke
                canvas.fill_rect(cx - kHandle, cy - kHandle,
                                 kHandle * 2, kHandle * 2);
                canvas.set_stroke_color(Color::rgba(0.0f, 0.0f, 0.0f, 0.6f));
                canvas.set_line_width(1.0f);
                canvas.stroke_rect(cx - kHandle, cy - kHandle,
                                   kHandle * 2, kHandle * 2);
            };
            paint_handle(r.x,             r.y,              DragCorner::nw);
            paint_handle(r.x + r.width,   r.y,              DragCorner::ne);
            paint_handle(r.x,             r.y + r.height,   DragCorner::sw);
            paint_handle(r.x + r.width,   r.y + r.height,   DragCorner::se);
        }

        // P2 grid guard affordance: when a body-drag was refused because
        // the selected view's parent is a grid container, surface a clear
        // "can't move grid child" badge near the top-left of the box so
        // the no-op isn't mysterious.
        if (move_refused_grid_) {
            const std::string msg = "grid child - move disabled";
            canvas.set_font("monospace", kFontSize);
            float tw = canvas.measure_text(msg);
            canvas.set_fill_color(Color::rgba(0.8f, 0.1f, 0.1f, 0.92f));
            canvas.fill_rounded_rect(r.x, r.y - 20, tw + 10, 16, 3);
            canvas.set_fill_color(Color::rgba(1, 1, 1, 1));
            canvas.fill_text(msg, r.x + 5, r.y - 7);
        }
    }
}

void InspectorOverlay::paint_drop_indicator(Canvas& canvas) {
    // Only while a reflow (non-float) move is dragging with a resolved drop.
    if (!move_active_ || move_float_ || !drop_target_) return;
    if (drop_indicator_.width <= 0 && drop_indicator_.height <= 0) return;

    const Rect& d = drop_indicator_;
    if (drop_indicator_is_line_) {
        // Blue insertion line between siblings (reorder).
        canvas.set_fill_color(Color::rgba(0.16f, 0.5f, 1.0f, 1.0f));
        canvas.fill_rect(d.x, d.y, std::max(2.0f, d.width),
                         std::max(2.0f, d.height));
    } else {
        // Translucent blue container highlight (drop-inside).
        canvas.set_fill_color(Color::rgba(0.16f, 0.5f, 1.0f, 0.18f));
        canvas.fill_rect(d.x, d.y, d.width, d.height);
        canvas.set_stroke_color(Color::rgba(0.16f, 0.5f, 1.0f, 0.95f));
        canvas.set_line_width(2.0f);
        canvas.stroke_rect(d.x, d.y, d.width, d.height);
    }
}

void InspectorOverlay::paint_distance_lines(Canvas& canvas) {
    // Helper: paint a single distance line + center-to-center px label
    // between two views. Returns early if either view is missing or the
    // two are the same.
    auto paint_one = [&](const View* a_view, const View* b_view) {
        if (!a_view || !b_view || a_view == b_view) return;

        auto a = view_bounds_in_root(a_view);
        auto b = view_bounds_in_root(b_view);

        float ax = a.x + a.width / 2;
        float ay = a.y + a.height / 2;
        float bx = b.x + b.width / 2;
        float by = b.y + b.height / 2;

        canvas.set_stroke_color(kDistanceLine);
        canvas.set_line_width(1.0f);
        canvas.stroke_line(ax, ay, bx, by);

        // Distance label
        float dx = bx - ax;
        float dy = by - ay;
        float dist = std::sqrt(dx * dx + dy * dy);
        auto label = std::to_string(static_cast<int>(dist)) + "px";
        float mx = (ax + bx) / 2;
        float my = (ay + by) / 2;

        canvas.set_font("monospace", kFontSize);
        canvas.set_fill_color(kDistanceLine);
        float tw = canvas.measure_text(label);
        canvas.fill_rounded_rect(mx - tw / 2 - 4, my - 8, tw + 8, 16, 3);
        canvas.set_fill_color(Color::rgba(1, 1, 1, 1));
        canvas.fill_text(label, mx - tw / 2, my + 4);
    };

    // Existing: Alt+click sticky distance-anchor mode
    paint_one(distance_anchor_, selected_);

    // Phase 3f: Alt-hover sibling distance (Figma-style spacing reveal).
    // While Alt is held during hover, dynamically paint a line from the
    // current selection to the view under the cursor. The two modes can
    // coexist — sticky anchor + live hover — for richer measurement.
    if (alt_hover_target_ && selected_ &&
        alt_hover_target_ != distance_anchor_) {
        paint_one(selected_, alt_hover_target_);
    }
}

void InspectorOverlay::paint_box_model(Canvas& canvas, const View* v) {
    if (!v || !v->parent()) return;

    auto r = view_bounds_in_root(v);
    auto& f = v->flex();

    // Padding (green, inside the view)
    float pt = f.padding_top >= 0 ? f.padding_top : f.padding;
    float pr = f.padding_right >= 0 ? f.padding_right : f.padding;
    float pb = f.padding_bottom >= 0 ? f.padding_bottom : f.padding;
    float pl = f.padding_left >= 0 ? f.padding_left : f.padding;
    if (pt > 0 || pr > 0 || pb > 0 || pl > 0) {
        canvas.set_fill_color(kPaddingColor);
        if (pt > 0) canvas.fill_rect(r.x, r.y, r.width, pt);
        if (pb > 0) canvas.fill_rect(r.x, r.y + r.height - pb, r.width, pb);
        if (pl > 0) canvas.fill_rect(r.x, r.y + pt, pl, r.height - pt - pb);
        if (pr > 0) canvas.fill_rect(r.x + r.width - pr, r.y + pt, pr, r.height - pt - pb);
    }

    // Margin (orange, outside the view)
    float mt = f.margin_t();
    float mr_ = f.margin_r();
    float mb = f.margin_b();
    float ml = f.margin_l();
    if (mt > 0 || mr_ > 0 || mb > 0 || ml > 0) {
        canvas.set_fill_color(kMarginColor);
        if (mt > 0) canvas.fill_rect(r.x - ml, r.y - mt, r.width + ml + mr_, mt);
        if (mb > 0) canvas.fill_rect(r.x - ml, r.y + r.height, r.width + ml + mr_, mb);
        if (ml > 0) canvas.fill_rect(r.x - ml, r.y, ml, r.height);
        if (mr_ > 0) canvas.fill_rect(r.x + r.width, r.y, mr_, r.height);
    }

    // Distance to parent
    auto parent_r = view_bounds_in_root(v->parent());
    float dist_top = r.y - parent_r.y;
    float dist_left = r.x - parent_r.x;
    float dist_bottom = (parent_r.y + parent_r.height) - (r.y + r.height);
    float dist_right = (parent_r.x + parent_r.width) - (r.x + r.width);

    canvas.set_font("monospace", 9.0f);
    canvas.set_fill_color(kDistanceLine);
    if (dist_top > 2) canvas.fill_text(std::to_string(static_cast<int>(dist_top)), r.x + r.width / 2, r.y - 3);
    if (dist_left > 2) canvas.fill_text(std::to_string(static_cast<int>(dist_left)), r.x - 20, r.y + r.height / 2);
    if (dist_bottom > 2) canvas.fill_text(std::to_string(static_cast<int>(dist_bottom)), r.x + r.width / 2, r.y + r.height + 10);
    if (dist_right > 2) canvas.fill_text(std::to_string(static_cast<int>(dist_right)), r.x + r.width + 3, r.y + r.height / 2);
}

void InspectorOverlay::paint_panel(Canvas& canvas) {
    float root_w = root_.bounds().width;
    float root_h = root_.bounds().height;
    float panel_x = root_w - panel_width_;

    // Panel background
    canvas.save();
    canvas.set_fill_color(kPanelBg);
    canvas.fill_rect(panel_x, 0, panel_width_, root_h);

    // Divider line
    canvas.set_stroke_color(Color::rgba(0.3f, 0.3f, 0.35f, 1.0f));
    canvas.set_line_width(1.0f);
    canvas.stroke_line(panel_x, 0, panel_x, root_h);

    float stats_y = root_h - kStatsBarHeight;

    // Helper: paint the middle "props" region. Phase 6.1 — when the
    // per-pass attribution viewer is toggled on (P-key), it takes over
    // this region instead of the property panel; the tree section above
    // is untouched so the user keeps navigation context. Phase 5.2 —
    // the reconciliation tab (R-key) takes over the same region with
    // the same discipline. Phase 6.2 — the texture-atlas viewer (A-key)
    // is the third tab to claim this region. Precedence when multiple
    // are toggled, oldest surface wins: pass viewer > reconciliation >
    // atlas viewer > props. The losers' cached row counts are reset so
    // reconcile_row_count() / atlas_row_count() never report a stale
    // layout for a tab that did not paint this frame.
    auto paint_middle = [&](float x, float y, float w, float h) {
        if (pass_viewer_enabled_) {
            reconcile_rows_.clear();
            atlas_row_count_ = 0;
            paint_pass_attribution(canvas, x, y, w, h);
        } else if (reconcile_tab_visible_) {
            atlas_row_count_ = 0;
            paint_reconcile_tab(canvas, x, y, w, h);
        } else if (atlas_viewer_visible_) {
            reconcile_rows_.clear();
            paint_atlas_tab(canvas, x, y, w, h);
        } else {
            reconcile_rows_.clear();
            atlas_row_count_ = 0;
            paint_props_section(canvas, x, y, w, h);
        }
    };

    if (tweaks_panel_visible_) {
        // Phase 2.5 layout: tree (top third), props (middle third),
        // tweaks management panel (bottom third). When the panel is
        // hidden the legacy two-section layout is used (below).
        float section_h = (stats_y) / 3.0f;

        float cursor_y = 4.0f;
        paint_tree_section(canvas, panel_x + 8, 4, panel_width_ - 16, cursor_y);

        float props_y = section_h;
        canvas.set_stroke_color(Color::rgba(0.3f, 0.3f, 0.35f, 0.5f));
        canvas.stroke_line(panel_x + 8, props_y, root_w - 8, props_y);

        // Phase 2 — drift drawer sits directly under the tree divider.
        // Paints nothing (and returns 0) when there is no drift, so the
        // props section is unaffected on the happy path.
        float drift_h = paint_drift_drawer(canvas, panel_x + 8, props_y + 4,
                                           panel_width_ - 16);
        paint_middle(panel_x + 8, props_y + 4 + drift_h,
                     panel_width_ - 16, section_h - 8 - drift_h);

        float tweaks_y = section_h * 2.0f;
        canvas.set_stroke_color(Color::rgba(0.3f, 0.3f, 0.35f, 0.5f));
        canvas.stroke_line(panel_x + 8, tweaks_y, root_w - 8, tweaks_y);
        paint_tweaks_section(canvas, panel_x + 8, tweaks_y + 4,
                             panel_width_ - 16, stats_y - tweaks_y - 8);
    } else {
        // Legacy two-section layout (pre-2.5).
        tweak_rows_.clear();
        float tree_height = root_h * 0.5f;
        float cursor_y = 4.0f;
        paint_tree_section(canvas, panel_x + 8, 4, panel_width_ - 16, cursor_y);

        float props_y = tree_height;
        canvas.set_stroke_color(Color::rgba(0.3f, 0.3f, 0.35f, 0.5f));
        canvas.stroke_line(panel_x + 8, props_y, root_w - 8, props_y);

        // Phase 2 — drift drawer sits directly under the tree divider.
        // Paints nothing (and returns 0) when there is no drift, so the
        // props section is unaffected on the happy path.
        float drift_h = paint_drift_drawer(canvas, panel_x + 8, props_y + 4,
                                           panel_width_ - 16);
        paint_middle(panel_x + 8, props_y + 4 + drift_h,
                     panel_width_ - 16, stats_y - props_y - 8 - drift_h);
    }

    // Stats bar (bottom)
    paint_stats_bar(canvas, panel_x, stats_y, panel_width_);

    canvas.restore();
}

void InspectorOverlay::paint_tree_section(Canvas& canvas, float x, float y, float w, float& cursor_y) {
    canvas.set_font("monospace", kFontSize);
    float tree_height = root_.bounds().height * 0.5f;

    for (auto& item : flat_tree_) {
        float row_y = y + cursor_y - tree_scroll_y_;
        if (row_y < y - kRowHeight || row_y > y + tree_height) {
            cursor_y += kRowHeight;
            continue;
        }

        float indent = item.depth * kIndent;

        // Highlight selected row
        if (item.view == selected_) {
            canvas.set_fill_color(kTreeSelected);
            canvas.fill_rect(x, row_y, w, kRowHeight);
        } else if (item.view == hovered_) {
            canvas.set_fill_color(kPanelHighlight);
            canvas.fill_rect(x, row_y, w, kRowHeight);
        }

        // Collapse indicator
        if (item.view->child_count() > 0) {
            canvas.set_fill_color(kPanelDim);
            auto indicator = collapsed_.count(item.view) ? "\xe2\x96\xb6" : "\xe2\x96\xbc";
            canvas.fill_text(indicator, x + indent, row_y + 14);
        }

        // Type name + optional ID
        auto type = ViewInspector::type_name(*item.view);
        auto id = item.view->id();
        std::string label = type;
        if (!id.empty()) label += " #" + id;

        canvas.set_fill_color(kPanelText);
        canvas.fill_text(label, x + indent + 14, row_y + 14);

        cursor_y += kRowHeight;
    }
}

void InspectorOverlay::paint_props_section(Canvas& canvas, float x, float y, float w, float h) {
    // Clear last frame's editable-field rects — repainted as we go.
    // The mouse handler reads this list during the SAME frame the user
    // clicked (paint runs on the UI thread before input dispatch).
    editable_fields_.clear();

    if (!selected_) {
        canvas.set_fill_color(kPanelDim);
        canvas.set_font("monospace", kFontSize);
        canvas.fill_text("Click a view to inspect", x, y + 16);
        return;
    }

    canvas.set_font("monospace", kFontSize);
    float line_y = y + 4;
    float line_h = 15.0f;

    // Phase 0b PR-C-2 — dot indicator for properties with local tweaks.
    // When the TweakStore (PR-A) has an entry for this view's anchor at
    // the given dotted path, paint a small orange dot in the gutter to
    // the LEFT of the label. The label/value text remains untouched so
    // the row still reads cleanly without color. The dot stays small
    // (3px radius) so the panel doesn't pulse with visual noise when
    // many tweaks are active. Designed to live next to (not over) the
    // existing label column at x.
    auto has_tweak = [&](std::string_view path) -> bool {
        if (!tweak_store_) return false;
        const auto& anchor = selected_->anchor_id();
        if (anchor.empty()) return false;
        if (tweak_store_->is_bypassed(anchor, path)) return false;
        return tweak_store_->lookup(anchor, path).has_value();
    };

    auto draw_label = [&](const std::string& label, const std::string& value,
                          std::string_view tweak_path = {}) {
        if (line_y > y + h) return;
        if (!tweak_path.empty() && has_tweak(tweak_path)) {
            // Orange dot indicator — same hue as the kSelectedStroke
            // selection color so users associate it visually with
            // "this is a Pulp-owned modification."
            canvas.set_fill_color(Color::rgba(1.0f, 0.5f, 0.0f, 0.9f));
            canvas.fill_circle(x - 6, line_y + 8, 3);
        }
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text(label, x, line_y + 11);
        canvas.set_fill_color(kPanelText);
        canvas.fill_text(value, x + 80, line_y + 11);
        line_y += line_h;
    };

    auto draw_heading = [&](const std::string& text) {
        if (line_y > y + h) return;
        line_y += 4;
        canvas.set_fill_color(kHighlightStroke);
        canvas.fill_text(text, x, line_y + 11);
        line_y += line_h;
    };

    // Phase 3b — emit one editable numeric row. Reserves a click-hit
    // rect in editable_fields_ keyed by dotted property path. If this
    // is the row currently being edited, draws the live edit_buffer_
    // text with a thin underline + caret instead of the static value.
    auto draw_editable = [&](const std::string& label,
                             const std::string& field_path,
                             float value,
                             const std::string& formatted_value) {
        if (line_y > y + h) return;
        // Field hit-rect — covers the entire value area, not the label,
        // so a click on "padding" text doesn't accidentally edit. Width
        // extends to the right edge of the panel section so the click
        // target is generous (Fitts's law).
        float value_x = x + 80;
        float value_w = (x + w) - value_x;
        Rect hit{value_x, line_y, value_w, line_h};
        editable_fields_.push_back({field_path, hit, value});

        // Phase 0b PR-C-2 — dot indicator coexists with Phase 3b edit
        // mode. The tweak-path here matches the field_path the editable
        // emits on commit, so an edit immediately shows a dot next time
        // the panel paints (and clears when bypassed). Drawn before the
        // label so the label/value text remains untouched.
        if (!field_path.empty() && has_tweak(field_path)) {
            canvas.set_fill_color(Color::rgba(1.0f, 0.5f, 0.0f, 0.9f));
            canvas.fill_circle(x - 6, line_y + 8, 3);
        }

        canvas.set_fill_color(kPanelDim);
        canvas.fill_text(label, x, line_y + 11);

        const bool editing_this = (editing_field_ == field_path);
        if (editing_this) {
            // Edit-mode background tint + underline + caret.
            canvas.set_fill_color(kFieldEditBg);
            canvas.fill_rect(value_x - 2, line_y, value_w + 4, line_h);

            // Show the live buffer.
            canvas.set_fill_color(kPanelText);
            const std::string& buf = edit_buffer_;
            canvas.fill_text(buf, value_x, line_y + 11);

            // Caret — width of "0" is a fine monospace approximation
            // since we set font("monospace", kFontSize). Measure the
            // prefix up to caret_pos for the X offset.
            std::string prefix = buf.substr(0, std::min(edit_caret_pos_, buf.size()));
            float caret_x = value_x + canvas.measure_text(prefix);
            canvas.set_stroke_color(kFieldEditCaret);
            canvas.set_line_width(1.0f);
            canvas.stroke_line(caret_x, line_y + 2, caret_x, line_y + line_h - 2);

            // Underline along the whole value area.
            canvas.set_stroke_color(kFieldEditUnder);
            canvas.set_line_width(1.0f);
            canvas.stroke_line(value_x, line_y + line_h - 1,
                               value_x + value_w, line_y + line_h - 1);
        } else {
            // Non-edit state: just the value text. The hit-rect is
            // invisible — Phase 3b intentionally keeps the chrome
            // minimal; a hover-cursor hint is future-work for the
            // platform host (see editable_field_at()).
            canvas.set_fill_color(kPanelText);
            canvas.fill_text(formatted_value, value_x, line_y + 11);
        }

        line_y += line_h;
    };

    // Type and ID
    auto type = ViewInspector::type_name(*selected_);
    draw_heading(type + (selected_->id().empty() ? "" : " #" + selected_->id()));

    // Phase 5.1 — authored-source row. When the selected view carries a
    // `__source` provenance record (set via the JS bridge's setSource()
    // for JSX-imported views), show "file:line" and a hint that the J
    // hotkey jumps to it. Hidden entirely for non-imported views so the
    // panel stays uncluttered.
    if (selected_->has_source_loc()) {
        const auto& loc = selected_->source_loc();
        std::string where = loc.file;
        if (loc.line > 0) where += ":" + std::to_string(loc.line);
        draw_label("source", where);
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("(press J to open in editor)", x, line_y + 11);
        line_y += line_h;
    }

    // Bounds (informational — bounds is layout OUTPUT, not editable.
    // Edits go through flex inputs below.)
    auto r = selected_->bounds();
    auto abs = view_bounds_in_root(selected_);
    draw_label("bounds", std::to_string(static_cast<int>(r.x)) + ", " +
               std::to_string(static_cast<int>(r.y)) + ", " +
               std::to_string(static_cast<int>(r.width)) + " × " +
               std::to_string(static_cast<int>(r.height)));
    draw_label("absolute", std::to_string(static_cast<int>(abs.x)) + ", " +
               std::to_string(static_cast<int>(abs.y)));

    // Visibility (not editable in Phase 3b — Phase 0b PR-C-2 adds dot)
    draw_label("visible", selected_->visible() ? "true" : "false", "paint.visible");

    // Phase 3b editable: opacity (always present, default 1.0).
    // Phase 0b PR-C-2: draw_editable now also paints the tweak dot when
    // style.opacity has a TweakStore entry.
    {
        float op = selected_->opacity();
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << op;
        draw_editable("opacity", "style.opacity", op, oss.str());
    }

    // Flex
    auto& f = selected_->flex();
    draw_heading("Layout");

    auto dir_str = [](FlexDirection d) -> std::string {
        switch (d) {
            case FlexDirection::row: return "row";
            case FlexDirection::column: return "column";
        }
        return "?";
    };
    draw_label("direction", dir_str(f.direction), "layout.direction");

    if (f.flex_grow > 0) draw_label("grow", std::to_string(f.flex_grow), "layout.grow");
    if (f.flex_shrink != 1.0f) draw_label("shrink", std::to_string(f.flex_shrink), "layout.shrink");
    if (f.gap > 0) draw_label("gap", std::to_string(static_cast<int>(f.gap)), "layout.gap");

    // Phase 3b editable: width / height — uses preferred_width /
    // preferred_height which are the Yoga flex INPUTS (Codex Phase 3
    // correction: set_bounds is an output that Yoga overwrites).
    {
        std::ostringstream oss;
        oss << static_cast<int>(f.preferred_width);
        draw_editable("width", "layout.width", f.preferred_width, oss.str());
    }
    {
        std::ostringstream oss;
        oss << static_cast<int>(f.preferred_height);
        draw_editable("height", "layout.height", f.preferred_height, oss.str());
    }

    // Phase 3b editable: padding (uniform). Per-side editing is a
    // future enhancement — exposed via the per-side rows below when
    // per-side overrides are already in use, otherwise the uniform
    // single field is the clean common case.
    {
        std::ostringstream oss;
        oss << static_cast<int>(f.padding);
        draw_editable("padding", "layout.padding", f.padding, oss.str());
    }

    // Phase 3b editable: margin (uniform).
    {
        std::ostringstream oss;
        oss << static_cast<int>(f.margin);
        draw_editable("margin", "layout.margin", f.margin, oss.str());
    }

    float pt2 = f.padding_top >= 0 ? f.padding_top : f.padding;
    float pr2 = f.padding_right >= 0 ? f.padding_right : f.padding;
    float pb2 = f.padding_bottom >= 0 ? f.padding_bottom : f.padding;
    float pl2 = f.padding_left >= 0 ? f.padding_left : f.padding;
    if ((pt2 != f.padding) || (pr2 != f.padding) ||
        (pb2 != f.padding) || (pl2 != f.padding)) {
        draw_label("padding (sides)",
                   std::to_string(static_cast<int>(pt2)) + " " +
                   std::to_string(static_cast<int>(pr2)) + " " +
                   std::to_string(static_cast<int>(pb2)) + " " +
                   std::to_string(static_cast<int>(pl2)),
                   "layout.padding");
    }

    // Theme colors (first 5)
    auto& theme = selected_->theme();
    if (!theme.colors.empty()) {
        draw_heading("Theme Colors");
        int shown = 0;
        for (auto& [name, color] : theme.colors) {
            if (shown >= 5) {
                draw_label("", "... +" + std::to_string(theme.colors.size() - 5) + " more");
                break;
            }
            // Color swatch
            if (line_y <= y + h) {
                canvas.set_fill_color(color);
                canvas.fill_rounded_rect(x, line_y + 3, 10, 10, 2);
                canvas.set_fill_color(kPanelDim);
                canvas.fill_text(name, x + 16, line_y + 11);
                line_y += line_h;
            }
            ++shown;
        }
    }
}

void InspectorOverlay::paint_stats_bar(Canvas& canvas, float x, float y, float w) {
    canvas.set_fill_color(kStatsBg);
    canvas.fill_rect(x, y, w, kStatsBarHeight);

    canvas.set_font("monospace", 10.0f);

    if (rpm_) {
        float frame_ms = rpm_->total_time_ms();
        float budget = rpm_->budget();
        bool over = rpm_->over_budget();

        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << frame_ms << "ms";
        if (budget > 0) {
            int fps = static_cast<int>(1000.0f / std::max(frame_ms, 0.1f));
            ss << "  " << fps << "fps";
        }

        canvas.set_fill_color(over ? kStatsWarn : kStatsText);
        canvas.fill_text(ss.str(), x + 8, y + 16);

        // Pass breakdown
        auto& passes = rpm_->passes();
        if (!passes.empty()) {
            std::string pass_info;
            for (auto& p : passes) {
                if (!pass_info.empty()) pass_info += " | ";
                pass_info += std::to_string(p.draw_calls) + "dc";
            }
            canvas.set_fill_color(kPanelDim);
            canvas.fill_text(pass_info, x + 140, y + 16);
        }
    } else {
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("No render stats", x + 8, y + 16);
    }

    // Phase 3c — active-mode hint. Drag (D) and eyedropper (E) are
    // mutually informative; show whichever is armed so the user
    // remembers a non-default canvas mode is in effect. Placed at the
    // bar midpoint so it never overlaps the left-aligned frame-time
    // readout or the right-aligned view count.
    if (eyedropper_active_ || dragging_enabled_) {
        canvas.set_fill_color(kFieldEditCaret);
        const char* mode = eyedropper_active_ ? "\xe2\x97\x89 eyedropper"
                                              : "\xe2\x97\x89 drag";
        canvas.fill_text(mode, x + w * 0.5f - 24.0f, y + 16);
    }

    // View count
    auto count = ViewInspector::count_views(root_);
    canvas.set_fill_color(kPanelDim);
    canvas.fill_text(std::to_string(count) + " views", x + w - 60, y + 16);
}

// Phase 6.1 — the per-pass GPU/render attribution viewer
// (capture_pass_frame / pass_attribution / paint_pass_attribution) is
// defined in inspector_overlay_pass_viewer.cpp.

// ── Phase 2.5 — Tweak management panel (Photoshop-layers style) ─────────────
//
// Lists every tweak in the attached TweakStore, grouped by anchor.
// Each tweak is a "layer" with three per-tweak controls:
//   eye   — bypass toggle (filled when active, hollow when bypassed)
//   lock  — protect from bulk-clear / reimport
//   trash — delete the tweak
// The row hit-rects are stashed in tweak_rows_ so the same-frame mouse
// handler can resolve a click. Anchor headers carry an abbreviated id;
// each row shows the dotted property path + a compact value preview.

namespace {

// Compact value preview for a tweak — keeps the row narrow. Numbers
// print without trailing-zero noise; strings clip to 16 chars.
std::string preview_value(const choc::value::Value& v) {
    if (v.isString()) {
        auto s = std::string(v.getString());
        if (s.size() > 16) s = s.substr(0, 15) + "\xe2\x80\xa6";
        return "\"" + s + "\"";
    }
    if (v.isBool()) return v.getBool() ? "true" : "false";
    if (v.isInt32() || v.isInt64())
        return std::to_string(v.getWithDefault<int64_t>(0));
    if (v.isFloat32() || v.isFloat64()) {
        std::ostringstream oss;
        oss << v.getWithDefault<double>(0.0);
        return oss.str();
    }
    if (v.isObject()) return "{\xe2\x80\xa6}";
    if (v.isArray()) return "[\xe2\x80\xa6]";
    return "?";
}

// Abbreviate an anchor id for the header — keep the tail (most
// distinctive) but cap total width so the header never overflows.
std::string abbreviate_anchor(const std::string& id) {
    constexpr std::size_t kMax = 22;
    if (id.size() <= kMax) return id;
    return "\xe2\x80\xa6" + id.substr(id.size() - (kMax - 1));
}

}  // namespace

void InspectorOverlay::paint_tweaks_section(Canvas& canvas, float x, float y,
                                            float w, float h) {
    // Repopulated every frame — the mouse handler reads it on the same
    // frame the user clicked (paint runs before input dispatch).
    tweak_rows_.clear();

    canvas.set_font("monospace", kFontSize);

    // Section heading.
    canvas.set_fill_color(kHighlightStroke);
    canvas.fill_text("Tweaks", x, y + 11);

    if (!tweak_store_) {
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("No tweak store attached", x, y + 11 + kRowHeight);
        return;
    }

    auto records = tweak_store_->list_tweaks();
    {
        std::ostringstream count_oss;
        count_oss << records.size() << (records.size() == 1 ? " tweak" : " tweaks");
        canvas.set_fill_color(kPanelDim);
        float cw = canvas.measure_text(count_oss.str());
        canvas.fill_text(count_oss.str(), x + w - cw, y + 11);
    }

    if (records.empty()) {
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("No tweaks recorded", x, y + 11 + kRowHeight);
        return;
    }

    // Group records by anchor — stable insertion order within an
    // anchor (list_tweaks() preserves it), anchors sorted so the panel
    // doesn't reshuffle across frames.
    std::vector<std::string> anchor_order;
    std::unordered_map<std::string, std::vector<const TweakStore::Record*>> grouped;
    for (auto& rec : records) {
        auto it = grouped.find(rec.anchor_id);
        if (it == grouped.end()) {
            grouped.emplace(rec.anchor_id,
                            std::vector<const TweakStore::Record*>{&rec});
            anchor_order.push_back(rec.anchor_id);
        } else {
            it->second.push_back(&rec);
        }
    }
    std::sort(anchor_order.begin(), anchor_order.end());

    // Clip the scroll region so rows don't bleed into the stats bar.
    canvas.save();
    float list_top = y + kRowHeight;
    canvas.clip_rect(x, list_top, w, h - kRowHeight);

    float row_y = list_top - tweaks_scroll_y_;
    constexpr float kIconSize = 14.0f;
    constexpr float kIconGap = 4.0f;
    // Three icons stacked at the right edge of the row.
    float icons_w = 3.0f * kIconSize + 2.0f * kIconGap;

    for (auto& anchor : anchor_order) {
        bool anchor_locked = tweak_store_->is_locked(anchor);

        // Anchor header row.
        if (row_y > list_top - kRowHeight && row_y < y + h) {
            canvas.set_fill_color(kPanelHighlight);
            canvas.fill_rect(x, row_y, w, kRowHeight);
            canvas.set_fill_color(kHighlightStroke);
            std::string hdr = abbreviate_anchor(anchor);
            if (anchor_locked) hdr = "\xf0\x9f\x94\x92 " + hdr;  // 🔒
            canvas.fill_text(hdr, x + 2, row_y + 14);
        }
        row_y += kRowHeight;

        for (const auto* rec : grouped[anchor]) {
            bool visible = row_y > list_top - kRowHeight && row_y < y + h;
            bool bypassed = tweak_store_->is_bypassed(rec->anchor_id,
                                                      rec->property_path);

            // Always record the hit-rects, even off-screen rows, so
            // tweak_row_count() is the true total; off-screen rects
            // simply never get clicked.
            float icons_x = x + w - icons_w;
            TweakRow hr;
            hr.anchor_id = rec->anchor_id;
            hr.property_path = rec->property_path;
            hr.bypass_icon = {icons_x, row_y + 3, kIconSize, kIconSize};
            hr.lock_icon   = {icons_x + kIconSize + kIconGap, row_y + 3,
                              kIconSize, kIconSize};
            hr.delete_icon = {icons_x + 2 * (kIconSize + kIconGap), row_y + 3,
                              kIconSize, kIconSize};
            tweak_rows_.push_back(hr);

            if (visible) {
                // Bypassed rows render dimmed to read like a hidden
                // Photoshop layer.
                const Color& path_col = bypassed ? kPanelDim : kPanelText;

                // Property path (indented under the anchor header).
                std::string label = rec->property_path;
                float max_label_w = (icons_x - kIconGap) - (x + kIndent);
                while (label.size() > 4 &&
                       canvas.measure_text(label + " = ") > max_label_w)
                    label = label.substr(0, label.size() - 1);
                canvas.set_fill_color(path_col);
                canvas.fill_text(label, x + kIndent, row_y + 14);

                // Value preview.
                std::string val = preview_value(rec->value);
                canvas.set_fill_color(kPanelDim);
                float label_w = canvas.measure_text(label + " ");
                canvas.fill_text("= " + val, x + kIndent + label_w, row_y + 14);

                // ── Icon: bypass (eye) ──────────────────────────────
                // Filled circle = visible/applied; hollow = bypassed.
                {
                    auto& r = hr.bypass_icon;
                    float cx = r.x + r.width / 2, cy = r.y + r.height / 2;
                    if (bypassed) {
                        canvas.set_stroke_color(kPanelDim);
                        canvas.set_line_width(1.5f);
                        canvas.stroke_line(r.x, r.y + r.height,
                                           r.x + r.width, r.y);
                        canvas.set_stroke_color(kPanelDim);
                        canvas.stroke_rect(r.x, r.y, r.width, r.height);
                    } else {
                        canvas.set_fill_color(kHighlightStroke);
                        canvas.fill_circle(cx, cy, r.width / 2 - 1);
                    }
                }
                // ── Icon: lock ──────────────────────────────────────
                {
                    auto& r = hr.lock_icon;
                    Color lock_col = anchor_locked
                        ? Color::rgba(1.0f, 0.78f, 0.2f, 1.0f)
                        : kPanelDim;
                    // Shackle (arc approximated by a rounded rect) +
                    // body box.
                    canvas.set_stroke_color(lock_col);
                    canvas.set_line_width(1.5f);
                    canvas.stroke_rect(r.x + 3, r.y + 1, r.width - 6,
                                       r.height / 2);
                    canvas.set_fill_color(lock_col);
                    canvas.fill_rounded_rect(r.x + 1, r.y + r.height / 2 - 1,
                                             r.width - 2, r.height / 2, 2);
                }
                // ── Icon: delete (trash) ────────────────────────────
                {
                    auto& r = hr.delete_icon;
                    canvas.set_stroke_color(
                        Color::rgba(1.0f, 0.4f, 0.35f, 1.0f));
                    canvas.set_line_width(1.5f);
                    // Lid.
                    canvas.stroke_line(r.x + 1, r.y + 3,
                                       r.x + r.width - 1, r.y + 3);
                    // Body box.
                    canvas.stroke_rect(r.x + 2, r.y + 3, r.width - 4,
                                       r.height - 4);
                    // Handle.
                    canvas.stroke_line(r.x + r.width / 2 - 2, r.y + 1,
                                       r.x + r.width / 2 + 2, r.y + 1);
                }
            }
            row_y += kRowHeight;
        }
    }

    canvas.restore();
}

InspectorOverlay::TweakAction
InspectorOverlay::tweak_action_at(Point p, std::size_t& out_row) const {
    auto hit = [&](const Rect& r) {
        return p.x >= r.x && p.x <= r.x + r.width &&
               p.y >= r.y && p.y <= r.y + r.height;
    };
    for (std::size_t i = 0; i < tweak_rows_.size(); ++i) {
        const auto& row = tweak_rows_[i];
        if (hit(row.bypass_icon)) { out_row = i; return TweakAction::bypass; }
        if (hit(row.lock_icon))   { out_row = i; return TweakAction::lock; }
        if (hit(row.delete_icon)) { out_row = i; return TweakAction::remove; }
    }
    return TweakAction::none;
}

// ── Phase 5.2 — Reconciliation tab ──────────────────────────────────────────
//
// A read-only report tab (R-key) showing, per tweak, whether the edit
// will survive a fresh design re-import. It classifies every stored
// tweak via reconcile_report() and renders one row each with a
// color-coded status badge. Like the Phase 6.1 pass viewer it takes
// over the property-panel region; unlike the tweak management panel it
// has no interactive controls — it is purely informational, so there
// are no hit-rects to record.

void InspectorOverlay::paint_reconcile_tab(Canvas& canvas, float x, float y,
                                           float w, float h) {
    // Status badge colors — green = reconciled (safe), amber = drift
    // (runtime-only), red = unresolvable (orphaned). The amber/red pair
    // matches the Phase 2 drift drawer so the two surfaces read
    // consistently.
    const Color kLockedColor = Color::rgba(0.35f, 0.85f, 0.45f, 1.0f);
    const Color kDriftColor  = Color::rgba(0.95f, 0.65f, 0.25f, 1.0f);
    const Color kUnresColor  = Color::rgba(0.95f, 0.40f, 0.38f, 1.0f);

    canvas.set_font("monospace", kFontSize);

    // Section heading.
    canvas.set_fill_color(kHighlightStroke);
    canvas.fill_text("Reconcile", x, y + 11);

    // Recompute the report fresh every frame — it is a cheap O(tweaks)
    // pass and the live tree / lock state may have changed since the
    // last paint. reconcile_rows_ caches the result for the row count.
    auto report = reconcile_report();
    reconcile_rows_ = report.rows;

    if (!tweak_store_) {
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("No tweak store attached", x, y + 11 + kRowHeight);
        return;
    }

    // Summary line: per-status counts, right-aligned in the header.
    {
        std::ostringstream summary;
        summary << report.locked_count << " locked  "
                << report.drifted_count << " drift  "
                << report.unresolvable_count << " unresolved";
        canvas.set_fill_color(kPanelDim);
        float sw = canvas.measure_text(summary.str());
        canvas.fill_text(summary.str(), x + w - sw, y + 11);
    }

    if (report.rows.empty()) {
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("No tweaks to reconcile", x, y + 11 + kRowHeight);
        return;
    }

    // Clip the scroll region so rows don't bleed past the section.
    canvas.save();
    float list_top = y + kRowHeight;
    canvas.clip_rect(x, list_top, w, h - kRowHeight);

    constexpr float kBadgeW = 16.0f;  // status pip diameter
    float row_y = list_top - reconcile_scroll_y_;

    for (const auto& row : report.rows) {
        const bool visible = row_y > list_top - kRowHeight && row_y < y + h;
        if (visible) {
            Color badge;
            const char* tag;
            switch (row.status) {
                case ReconcileStatus::locked_to_source:
                    badge = kLockedColor; tag = "lock"; break;
                case ReconcileStatus::drifted:
                    badge = kDriftColor;  tag = "drift"; break;
                case ReconcileStatus::unresolvable:
                default:
                    badge = kUnresColor;  tag = "orphan"; break;
            }

            // Status pip at the left edge.
            canvas.set_fill_color(badge);
            canvas.fill_circle(x + 5.0f, row_y + kRowHeight / 2.0f, 4.0f);

            // Anchor + property path, dimmed for unresolvable rows so
            // an orphaned tweak reads as "inactive" at a glance.
            const bool orphan = row.status == ReconcileStatus::unresolvable;
            std::string label = abbreviate_anchor(row.anchor_id) + "  " +
                                 row.property_path;
            float text_x = x + kBadgeW;
            float tag_w = canvas.measure_text(tag) + 6.0f;
            float max_label_w = (x + w - tag_w) - text_x;
            while (label.size() > 4 &&
                   canvas.measure_text(label) > max_label_w)
                label = label.substr(0, label.size() - 1);
            canvas.set_fill_color(orphan ? kPanelDim : kPanelText);
            canvas.fill_text(label, text_x, row_y + 14);

            // Status tag, right-aligned, in the badge color.
            canvas.set_fill_color(badge);
            canvas.fill_text(tag, x + w - canvas.measure_text(tag),
                             row_y + 14);
        }
        row_y += kRowHeight;
    }

    canvas.restore();
}

// ── Phase 6.2 — Texture atlas viewer ────────────────────────────────────────
//
// A read-only GPU-perf observability tab that answers "is my SDF atlas
// thrashing?". It renders the render layer's texture-atlas inventory —
// per-atlas dimensions, page count, live entry count, and a shelf-packer
// occupancy bar. Like the Phase 6.1 pass viewer and Phase 5.2
// reconciliation tab it takes over the property-panel region; it has no
// interactive controls (purely informational), so there are no hit-rects
// to record. Degrades gracefully to a "GPU atlas unavailable" line when
// no inventory is wired (headless / GPU-off builds).

void InspectorOverlay::paint_atlas_tab(Canvas& canvas, float x, float y,
                                       float w, float h) {
    // Occupancy bar colors: green when there's headroom, amber when the
    // atlas is filling, red when nearly full (thrash risk). The amber/red
    // pair matches the drift drawer + reconciliation tab so the GPU-perf
    // surfaces read consistently.
    const Color kBarTrack = Color::rgba(0.20f, 0.20f, 0.24f, 1.0f);
    const Color kBarLow   = Color::rgba(0.35f, 0.85f, 0.45f, 1.0f);
    const Color kBarMid   = Color::rgba(0.95f, 0.65f, 0.25f, 1.0f);
    const Color kBarHigh  = Color::rgba(0.95f, 0.40f, 0.38f, 1.0f);

    canvas.set_font("monospace", kFontSize);

    // Section heading.
    canvas.set_fill_color(kHighlightStroke);
    canvas.fill_text("Texture Atlases (A)", x, y + 11);

    // No inventory wired — graceful empty state. This is the headless /
    // GPU-off path; the tab must never crash here.
    if (!atlas_inventory_ || atlas_inventory_->empty()) {
        atlas_row_count_ = 0;
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("GPU atlas unavailable", x, y + 11 + kRowHeight);
        return;
    }

    const auto& atlases = atlas_inventory_->atlases();
    atlas_row_count_ = atlases.size();

    // Summary line: total pages + total packed entries, right-aligned in
    // the header — mirrors the reconciliation tab's per-status summary.
    {
        std::ostringstream summary;
        summary << atlas_inventory_->total_pages() << " pages  "
                << atlas_inventory_->total_entries() << " entries";
        canvas.set_fill_color(kPanelDim);
        float sw = canvas.measure_text(summary.str());
        canvas.fill_text(summary.str(), x + w - sw, y + 11);
    }

    // Each atlas occupies a two-line row: a label/dimensions/entries
    // line, then an occupancy bar with a percentage readout.
    constexpr float kAtlasRowH = kRowHeight * 2.0f + 6.0f;

    canvas.save();
    float list_top = y + kRowHeight;
    canvas.clip_rect(x, list_top, w, h - kRowHeight);

    float row_y = list_top - atlas_scroll_y_;
    for (const auto& a : atlases) {
        const bool visible = row_y > list_top - kAtlasRowH && row_y < y + h;
        if (visible) {
            // Line 1: "<label>  <W>x<H>" left, "<N> entries" right.
            std::ostringstream dims;
            dims << a.label << "  " << a.width << "x" << a.height;
            if (a.pages > 1) dims << " x" << a.pages << "p";
            canvas.set_fill_color(kPanelText);
            canvas.fill_text(dims.str(), x, row_y + 12);

            std::ostringstream ent;
            ent << a.entries << " entries";
            canvas.set_fill_color(kPanelDim);
            float ew = canvas.measure_text(ent.str());
            canvas.fill_text(ent.str(), x + w - ew, row_y + 12);

            // Line 2: occupancy bar + percent readout.
            const int pct = a.occupancy_percent();
            const Color& fill = pct >= 85 ? kBarHigh
                              : pct >= 60 ? kBarMid
                                          : kBarLow;
            const float bar_y = row_y + kRowHeight;
            const float bar_h = 8.0f;
            std::ostringstream pctstr;
            pctstr << pct << "%";
            const float pct_w = canvas.measure_text(pctstr.str()) + 6.0f;
            const float bar_w = w - pct_w;

            canvas.set_fill_color(kBarTrack);
            canvas.fill_rounded_rect(x, bar_y, bar_w, bar_h, 2.0f);
            float frac = static_cast<float>(pct) / 100.0f;
            if (frac > 0.0f) {
                canvas.set_fill_color(fill);
                canvas.fill_rounded_rect(x, bar_y,
                                         std::max(2.0f, bar_w * frac),
                                         bar_h, 2.0f);
            }
            canvas.set_fill_color(fill);
            canvas.fill_text(pctstr.str(), x + w - pct_w + 6.0f,
                             bar_y + bar_h);
        }
        row_y += kAtlasRowH;
    }

    canvas.restore();
}

// ── Phase 2 — Drift drawer ──────────────────────────────────────────────────
//
// A collapsible warning panel that lists tweaks whose anchor / property
// no longer maps to the live design. Header is always shown when drift
// exists (so the count badge is visible); the body — one row per
// orphaned/drifted tweak — only renders when expanded. Each row shows
// the anchor, the dotted property path, the stored value, and a reason
// tag. Clicking the header chevron toggles the drawer.

float InspectorOverlay::paint_drift_drawer(Canvas& canvas, float x, float y,
                                           float w) {
    drift_header_hit_ = {};  // reset; repopulated below if we paint.
    if (drifted_.empty()) return 0.0f;

    const Color kDriftBg     = Color::rgba(0.22f, 0.06f, 0.07f, 0.95f);
    const Color kDriftBorder = Color::rgba(0.95f, 0.32f, 0.30f, 0.85f);
    const Color kDriftText   = Color::rgba(0.98f, 0.72f, 0.70f, 1.0f);
    const Color kDriftReason = Color::rgba(0.95f, 0.55f, 0.30f, 1.0f);

    constexpr float kHeaderH = 22.0f;
    constexpr float kDriftRowH = 30.0f;
    // Cap the body so a large drift list never eats the whole panel.
    const std::size_t kMaxRows = 6;
    const std::size_t shown_rows =
        drift_drawer_open_ ? std::min(drifted_.size(), kMaxRows) : 0;
    const bool truncated = drifted_.size() > kMaxRows;
    float body_h = static_cast<float>(shown_rows) * kDriftRowH;
    if (drift_drawer_open_ && truncated) body_h += 16.0f;  // "+N more" line
    float total_h = kHeaderH + body_h + 4.0f;

    canvas.set_font("monospace", kFontSize);

    // ── Header ──────────────────────────────────────────────────────
    canvas.set_fill_color(kDriftBg);
    canvas.fill_rect(x, y, w, kHeaderH);
    canvas.set_stroke_color(kDriftBorder);
    canvas.set_line_width(1.0f);
    canvas.stroke_rect(x, y, w, kHeaderH);

    // The whole header is the toggle target (generous hit box).
    drift_header_hit_ = {x, y, w, kHeaderH};

    auto chevron = drift_drawer_open_ ? "\xe2\x96\xbc" : "\xe2\x96\xb6";
    canvas.set_fill_color(kDriftBorder);
    canvas.fill_text(chevron, x + 4, y + 15);

    std::string title = "\xe2\x9a\xa0 Drift — " +
                        std::to_string(drifted_.size()) +
                        (drifted_.size() == 1 ? " orphaned tweak"
                                              : " orphaned tweaks");
    canvas.set_fill_color(kDriftText);
    canvas.fill_text(title, x + 18, y + 15);

    if (!drift_drawer_open_) return total_h;

    // ── Body — one row per drifted/orphaned tweak ──────────────────
    canvas.set_fill_color(Color::rgba(0.14f, 0.05f, 0.06f, 0.95f));
    canvas.fill_rect(x, y + kHeaderH, w, body_h);

    float row_y = y + kHeaderH;
    for (std::size_t i = 0; i < shown_rows; ++i) {
        const auto& d = drifted_[i];

        // Left red marker stripe — matches Phase 2.5's planned
        // drift-row styling so the two panels read consistently.
        canvas.set_fill_color(kDriftBorder);
        canvas.fill_rect(x, row_y + 2, 2.0f, kDriftRowH - 4);

        // Line 1: anchor + reason tag.
        std::string anchor = d.anchor_id;
        if (anchor.size() > 28) anchor = anchor.substr(0, 27) + "\xe2\x80\xa6";
        canvas.set_fill_color(kDriftText);
        canvas.fill_text(anchor, x + 8, row_y + 13);

        std::string reason = TweakStore::drift_reason_str(d.reason);
        canvas.set_fill_color(kDriftReason);
        float rw = canvas.measure_text(reason);
        canvas.fill_text(reason, x + w - rw - 4, row_y + 13);

        // Line 2: property path = stored value.
        std::string value_str;
        if (d.value.isString()) {
            value_str = std::string(d.value.getString());
        } else {
            try {
                value_str = choc::json::toString(d.value);
            } catch (...) {
                value_str = "?";
            }
        }
        std::string detail = d.property_path + " = " + value_str;
        if (detail.size() > 40) detail = detail.substr(0, 39) + "\xe2\x80\xa6";
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text(detail, x + 8, row_y + 25);

        row_y += kDriftRowH;
    }

    if (truncated) {
        canvas.set_fill_color(kPanelDim);
        canvas.fill_text("\xe2\x80\xa6 +" +
                             std::to_string(drifted_.size() - kMaxRows) +
                             " more (see `pulp tweaks diff`)",
                         x + 8, row_y + 12);
    }

    return total_h;
}

// Phase 3b — the live-editable box-model fields (editable_field_at /
// read_field_value / write_field_value / begin_field_edit /
// commit_field_edit / cancel_field_edit / apply_edit_buffer_to_view /
// handle_edit_key) are defined in inspector_overlay_field_edit.cpp.

// Phase 3e — the 20× zoom loupe (set_zoom_active / set_zoom_factor /
// resolve_view_color_at / update_zoom_sample / paint_zoom_panel) is
// defined in inspector_overlay_zoom.cpp.

} // namespace pulp::inspect
