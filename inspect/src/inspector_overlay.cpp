// inspector_overlay.cpp — Visual inspector overlay implementation
//
// Per-feature overlay clusters live in sibling translation units:
//   inspector_overlay_field_edit.cpp   — live-editable box-model fields
//   inspector_overlay_paint.cpp        — overlay and panel painting
//   inspector_overlay_zoom.cpp         — 20× zoom loupe
//   inspector_overlay_pass_viewer.cpp  — per-pass attribution viewer
// Shared overlay color constants are declared in
// inspector_overlay_internal.hpp.

#include "inspector_overlay_internal.hpp"

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/widgets.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/render/render_pass.hpp>
#include <pulp/render/atlas_inventory.hpp>
#include <pulp/runtime/log.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <utility>

namespace pulp::inspect {

// ── Constructor ─────────────────────────────────────────────────────────────

InspectorOverlay::InspectorOverlay(View& root) : root_(root) {}

// Move-tweak namespace bridge: map a `layout.{position,left,top}` tweak
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

BadgePlacement compute_badge_placement(float sel_x, float sel_y, float sel_h,
                                       float badge_w, float badge_h,
                                       float root_w,
                                       float gap, float top_margin) {
    BadgePlacement out;
    float above_y = sel_y - gap - badge_h;
    if (above_y < top_margin) {
        // Not enough room above — flip below the selection.
        out.y = sel_y + sel_h + gap;
        out.below = true;
    } else {
        out.y = above_y;
        out.below = false;
    }

    // Clamp x to keep the badge on-screen left/right.
    float x = sel_x;
    if (x < 0.0f) x = 0.0f;
    if (root_w > 0.0f && x + badge_w > root_w) x = root_w - badge_w;
    if (x < 0.0f) x = 0.0f;  // badge wider than root: pin to left edge
    out.x = x;
    return out;
}

void install_inspector_hooks(InspectorOverlay& inspector) {
    g_active_inspector = &inspector;
    // Install all hooks via function pointers — no circular dependency.
    // Every hook dereferences g_active_inspector rather than capturing the
    // InspectorOverlay by reference, so uninstall_inspector_hooks() (or a bare
    // `g_active_inspector = nullptr` on an old teardown path) makes the still-
    // installed hooks inert instead of firing on a destroyed inspector. A null
    // global returns each hook's no-op default (skip paint / don't handle / -1).
    //
    // Gate overlay paint on the inspected root. The overlay's
    // selection box / handles / drop indicators are positioned in the inspected
    // root's coordinate space, so they must paint ONLY when the root being
    // painted is the inspected root. Without this gate the same overlay paints
    // into the floating InspectorWindow's own root surface at the overlay's
    // root coordinates — a stray box at a random spot inside the inspector
    // window. nullptr (root unknown, legacy caller) paints unconditionally.
    View::set_inspector_paint_hook(
        [](Canvas& canvas, View* painting_root) {
            InspectorOverlay* insp = g_active_inspector;
            if (!insp) return;
            if (painting_root && painting_root != &insp->inspected_root())
                return;
            insp->paint(canvas);
        });
    View::set_inspector_key_hook([](const KeyEvent& e) -> bool {
        InspectorOverlay* insp = g_active_inspector;
        return insp ? insp->handle_key_event(e) : false;
    });
    // Window-gate the mouse hook, mirroring the paint-hook gate above. A
    // secondary window (the floating InspectorWindow) routes its
    // own root as `event_root`; only events whose root is the inspected canvas
    // root may drive the overlay, otherwise hovering/clicking/dragging inside
    // the inspector window would highlight/affect the canvas. nullptr (root
    // unknown, legacy/headless caller) runs unconditionally.
    View::set_inspector_mouse_hook(
        [](const MouseEvent& e, View* event_root) -> bool {
            InspectorOverlay* insp = g_active_inspector;
            if (!insp) return false;
            if (event_root && event_root != &insp->inspected_root())
                return false;
            return insp->handle_mouse_event(e);
        });
    // Install the inline-text-edit hook here so the standalone host (and any
    // other install_inspector_hooks() caller) can
    // actually deliver typed characters into a Text-tool edit. Root-gated like
    // the mouse/cursor hooks: text typed into a
    // secondary window (the floating InspectorWindow) must not drive the
    // canvas overlay's inline edit. nullptr (root unknown, legacy/headless
    // caller) runs unconditionally.
    View::set_inspector_text_hook(
        [](const TextInputEvent& e, View* event_root) -> bool {
            InspectorOverlay* insp = g_active_inspector;
            if (!insp) return false;
            if (event_root && event_root != &insp->inspected_root())
                return false;
            return insp->handle_text_input(e);
        });
    // Cursor-affordance hook for move/resize cursor feedback over a selected
    // element. Same root gate; returns -1 to defer to the
    // normal hit-view cursor() path when off the selection or in another
    // window.
    View::set_inspector_cursor_hook(
        [](const MouseEvent& e, View* event_root) -> int {
            InspectorOverlay* insp = g_active_inspector;
            if (!insp) return -1;
            if (event_root && event_root != &insp->inspected_root())
                return -1;
            return insp->cursor_style_for(e.position);
        });
}

void uninstall_inspector_hooks() {
    // Clear the global first so any hook that fires between the two steps sees
    // a null inspector and no-ops, then release the five View hook slots so no
    // stale std::function outlives the inspector it was installed for.
    g_active_inspector = nullptr;
    View::set_inspector_paint_hook({});
    View::set_inspector_key_hook({});
    View::set_inspector_mouse_hook({});
    View::set_inspector_text_hook({});
    View::set_inspector_cursor_hook({});
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
        // Drop any in-progress inline text edit so deactivating the
        // inspector (incl. the Esc-deselect set_active(false)/(true)
        // cycle) never leaves a dangling text_edit_target_.
        if (text_editing()) cancel_text_edit();
        selected_ = nullptr;
        hovered_ = nullptr;
        alt_hover_target_ = nullptr;
        distance_anchor_ = nullptr;
        editable_fields_.clear();
        // Drop any pending eyedropper state with the
        // inspector so a stale swatch never paints on the next open.
        eyedropper_active_ = false;
        eyedropper_has_sample_ = false;
        // The loupe is a transient inspect tool; dismissing
        // the whole inspector also closes it so re-opening starts clean.
        zoom_active_ = false;
        // Drop the reconciliation tab's laid-out rows so
        // reconcile_row_count() reports 0 while the inspector is shut.
        // The R-key toggle state itself is left intact (mirrors how
        // the tweaks panel keeps tweaks_panel_visible_ across opens).
        reconcile_rows_.clear();
        // Reset the atlas viewer's laid-out row count so
        // atlas_row_count() reports 0 while the inspector is shut. As
        // with the reconciliation tab the `A`-key toggle state is left
        // intact so re-opening the inspector restores the same tab.
        atlas_row_count_ = 0;
    }
}

// ── Color eyedropper ────────────────────────────────────────────────────────
//
// Eyedropper mode (E key, alongside D for drag handles) lets the user
// sample a color from the rendered UI and
// apply it as a tweak to the selected view's color property.
//
// Two sampling paths, picked at runtime:
//   1. Framebuffer readback — `Canvas::read_pixels()` returns the
//      exact rendered pixel under the cursor. Implemented only on
//      Skia raster surfaces; when present
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
// — the same path as other overlay gestures — encoded as a "#rrggbb"
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

// ── In-process gesture-tweak emission ──────────────────────────────────────
//
// Routes overlay-driven direct-manipulation edits (drag handles, color
// pick, field edit) to
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

// ── Figma-style tool palette + inline text editing ─────────────────────────

void InspectorOverlay::set_tool(Tool t) {
    if (tool_ == t) return;
    // Leaving the Text tool with an edit open commits it (the user
    // deliberately switched tools — treat it like click-away-to-commit
    // rather than silently dropping the edit).
    if (tool_ == Tool::text && t != Tool::text && text_editing())
        commit_text_edit();
    tool_ = t;
}

bool InspectorOverlay::view_has_editable_text(const View* v) {
    if (!v) return false;
    return dynamic_cast<const Label*>(v) != nullptr ||
           dynamic_cast<const TextEditor*>(v) != nullptr;
}

std::string InspectorOverlay::editable_text_of(const View* v) {
    if (!v) return {};
    if (auto* lbl = dynamic_cast<const Label*>(v)) return lbl->text();
    if (auto* ed = dynamic_cast<const TextEditor*>(v)) return ed->text();
    return {};
}

void InspectorOverlay::set_editable_text_of(View* v, const std::string& text) {
    if (!v) return;
    if (auto* lbl = dynamic_cast<Label*>(v)) {
        lbl->set_text(text);
        // Text length changes intrinsic size — reflow up to the root so
        // the new copy lays out + repaints on the next pass.
        for (View* p = v; p; p = p->parent()) p->invalidate_layout();
        return;
    }
    if (auto* ed = dynamic_cast<TextEditor*>(v)) {
        ed->set_text(text);
        for (View* p = v; p; p = p->parent()) p->invalidate_layout();
    }
}

// Confirm text_edit_target_ is still attached to the live
// tree by walking from the root and comparing pointers. This NEVER
// dereferences text_edit_target_ itself, so it is safe even if the target was
// freed: a freed pointer simply won't be found among the live nodes. Returns
// false when not editing.
bool InspectorOverlay::text_edit_target_reachable() const {
    if (!text_edit_target_) return false;
    const View* target = text_edit_target_;
    std::function<bool(const View*)> contains = [&](const View* v) -> bool {
        if (v == target) return true;
        for (std::size_t i = 0; i < v->child_count(); ++i)
            if (contains(v->child_at(i))) return true;
        return false;
    };
    return contains(&root_);
}

// Drop the inline-text-edit state without touching the
// (possibly freed) target view. Used when the target leaves the tree.
void InspectorOverlay::clear_text_edit_state() {
    text_edit_target_ = nullptr;
    text_edit_buffer_.clear();
    text_edit_original_.clear();
    text_caret_ = 0;          // drop caret/selection too
    text_sel_anchor_ = 0;
}

// Re-find a live view by stable anchor id (or nullptr).
// EditHistory closures call this at undo/redo time so an anchored target that
// survived a subtree rebuild resolves to the CURRENT live view rather than a
// dangling raw pointer captured at gesture time.
View* InspectorOverlay::resolve_anchor(const std::string& anchor) const {
    if (anchor.empty()) return nullptr;
    return ViewInspector::find_by_anchor(root_, anchor);
}

// Return the anchor of the visible sibling immediately preceding
// `child` under `parent` in flex-order, or "" for first-child / no parent /
// un-anchored preceding sibling. Mirrors commit_reflow_drop's sibling ordering
// (visible children sorted by flex().order, stable).
std::string InspectorOverlay::preceding_sibling_anchor(const View* parent,
                                                       const View* child) {
    if (!parent || !child) return {};
    std::vector<const View*> sibs;
    for (std::size_t i = 0; i < parent->child_count(); ++i) {
        const View* c = parent->child_at(i);
        if (!c->visible()) continue;
        sibs.push_back(c);
    }
    std::stable_sort(sibs.begin(), sibs.end(),
        [](const View* a, const View* b) {
            return a->flex().order < b->flex().order;
        });
    const View* prev = nullptr;
    for (const View* c : sibs) {
        if (c == child) break;
        prev = c;
    }
    if (!prev) return {};                 // child is first → first-child slot
    return prev->anchor_id();             // "" if the preceding sibling is un-anchored
}

// Register a raw View* that an EditHistory closure captured
// because the view has NO anchor to re-find it by. De-duplicates so the list
// stays small across coalesced gestures.
void InspectorOverlay::track_raw_history_view(View* v) {
    if (!v) return;
    for (View* existing : raw_history_views_)
        if (existing == v) return;
    raw_history_views_.push_back(v);
}

// True if any tracked raw-pointer view has left the tree
// (so its captured closures would deref freed memory). Pointer-compare only —
// never dereferences a tracked pointer.
bool InspectorOverlay::any_raw_history_view_dangling() const {
    if (raw_history_views_.empty()) return false;
    auto in_tree = [&](const View* target) {
        std::function<bool(const View*)> contains = [&](const View* v) -> bool {
            if (v == target) return true;
            for (std::size_t i = 0; i < v->child_count(); ++i)
                if (contains(v->child_at(i))) return true;
            return false;
        };
        return contains(&root_);
    };
    for (View* v : raw_history_views_)
        if (!in_tree(v)) return true;
    return false;
}

bool InspectorOverlay::begin_text_edit(View* v) {
    if (!view_has_editable_text(v)) return false;
    // Selecting the edit target keeps the selection box + props panel on
    // it (and lets emit_tweak_for_selection() resolve its anchor).
    selected_ = v;
    text_edit_target_ = v;
    text_edit_original_ = editable_text_of(v);
    text_edit_buffer_ = text_edit_original_;
    // Put the caret at the end of the seeded text, with no selection, and reset
    // blink state so the caret is immediately visible on entry.
    text_caret_ = text_edit_buffer_.size();
    text_sel_anchor_ = text_caret_;
    text_blink_ticks_ = 0;
    return true;
}

// ── UTF-8 caret/selection helpers ──────────────────────────────────────────
//
// The edit buffer is a UTF-8 std::string. Caret offsets are byte indices kept
// on codepoint boundaries. These mirror TextEditor's manipulation logic
// (caret index, selection range, word/line moves, clipboard) without swapping
// in the TextEditor widget chrome — the live View text keeps the real-UI look.

namespace {

// True if byte `b` is a UTF-8 continuation byte (10xxxxxx).
inline bool is_utf8_cont(unsigned char b) { return (b & 0xC0) == 0x80; }

// Step one codepoint left of byte offset `i` in `s` (clamped at 0).
std::size_t utf8_prev(const std::string& s, std::size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && is_utf8_cont(static_cast<unsigned char>(s[i]))) --i;
    return i;
}

// Step one codepoint right of byte offset `i` in `s` (clamped at size()).
std::size_t utf8_next(const std::string& s, std::size_t i) {
    const std::size_t n = s.size();
    if (i >= n) return n;
    ++i;
    while (i < n && is_utf8_cont(static_cast<unsigned char>(s[i]))) ++i;
    return i;
}

bool is_word_byte(unsigned char c) {
    return std::isalnum(c) || c == '_' || c >= 0x80;  // treat non-ASCII as word
}

}  // namespace

std::pair<std::size_t, std::size_t> InspectorOverlay::text_selection() const {
    return {std::min(text_caret_, text_sel_anchor_),
            std::max(text_caret_, text_sel_anchor_)};
}

void InspectorOverlay::text_move_caret(int delta, bool extend) {
    if (!text_editing()) return;
    const std::string& s = text_edit_buffer_;
    // With a selection and no extend, a left/right arrow collapses to the
    // selection edge (the standard editor behavior) rather than moving past.
    if (!extend && text_has_selection()) {
        auto [lo, hi] = text_selection();
        text_caret_ = (delta < 0) ? lo : hi;
        text_sel_anchor_ = text_caret_;
        // A single arrow after collapsing does not also move.
        if ((delta < 0 && text_caret_ == lo) || (delta > 0 && text_caret_ == hi)) {
            text_blink_ticks_ = 0;
            return;
        }
    }
    int steps = std::abs(delta);
    for (int k = 0; k < steps; ++k) {
        text_caret_ = (delta < 0) ? utf8_prev(s, text_caret_)
                                  : utf8_next(s, text_caret_);
    }
    if (!extend) text_sel_anchor_ = text_caret_;
    text_blink_ticks_ = 0;
}

void InspectorOverlay::text_move_word(int direction, bool extend) {
    if (!text_editing()) return;
    const std::string& s = text_edit_buffer_;
    std::size_t i = text_caret_;
    if (direction < 0) {
        // Skip whitespace/punctuation left, then the word run left.
        while (i > 0 && !is_word_byte(static_cast<unsigned char>(s[i - 1]))) --i;
        while (i > 0 && is_word_byte(static_cast<unsigned char>(s[i - 1]))) --i;
    } else {
        const std::size_t n = s.size();
        while (i < n && !is_word_byte(static_cast<unsigned char>(s[i]))) ++i;
        while (i < n && is_word_byte(static_cast<unsigned char>(s[i]))) ++i;
    }
    text_caret_ = i;
    if (!extend) text_sel_anchor_ = i;
    text_blink_ticks_ = 0;
}

void InspectorOverlay::text_move_home(bool extend) {
    if (!text_editing()) return;
    text_caret_ = 0;
    if (!extend) text_sel_anchor_ = 0;
    text_blink_ticks_ = 0;
}

void InspectorOverlay::text_move_end(bool extend) {
    if (!text_editing()) return;
    text_caret_ = text_edit_buffer_.size();
    if (!extend) text_sel_anchor_ = text_caret_;
    text_blink_ticks_ = 0;
}

void InspectorOverlay::text_select_all() {
    if (!text_editing()) return;
    text_sel_anchor_ = 0;
    text_caret_ = text_edit_buffer_.size();
    text_blink_ticks_ = 0;
}

bool InspectorOverlay::text_copy() {
    if (!text_editing() || !text_has_selection()) return false;
    auto [lo, hi] = text_selection();
    return pulp::platform::Clipboard::set_text(
        text_edit_buffer_.substr(lo, hi - lo));
}

bool InspectorOverlay::text_cut() {
    if (!text_editing() || !text_has_selection()) return false;
    auto [lo, hi] = text_selection();
    const bool ok = pulp::platform::Clipboard::set_text(
        text_edit_buffer_.substr(lo, hi - lo));
    // Cut removes the selection even if the system clipboard write failed
    // (Linux without xclip/wl-copy) — matching TextEditor, the edit-local
    // delete is the user-visible action.
    text_edit_buffer_.erase(lo, hi - lo);
    text_caret_ = lo;
    text_sel_anchor_ = lo;
    if (text_edit_target_reachable())
        set_editable_text_of(text_edit_target_, text_edit_buffer_);
    text_blink_ticks_ = 0;
    return ok;
}

bool InspectorOverlay::text_paste() {
    if (!text_editing()) return false;
    auto clip = pulp::platform::Clipboard::get_text();
    if (!clip || clip->empty()) return false;
    text_insert(*clip);
    return true;
}

void InspectorOverlay::text_insert(const std::string& utf8) {
    if (!text_editing()) return;
    if (!text_edit_target_reachable()) { clear_text_edit_state(); return; }
    // Replace any selection first.
    if (text_has_selection()) {
        auto [lo, hi] = text_selection();
        text_edit_buffer_.erase(lo, hi - lo);
        text_caret_ = lo;
        text_sel_anchor_ = lo;
    }
    if (text_caret_ > text_edit_buffer_.size())
        text_caret_ = text_edit_buffer_.size();
    text_edit_buffer_.insert(text_caret_, utf8);
    text_caret_ += utf8.size();
    text_sel_anchor_ = text_caret_;
    set_editable_text_of(text_edit_target_, text_edit_buffer_);
    text_blink_ticks_ = 0;
}

void InspectorOverlay::text_delete_backward() {
    if (!text_editing()) return;
    if (!text_edit_target_reachable()) { clear_text_edit_state(); return; }
    if (text_has_selection()) {
        auto [lo, hi] = text_selection();
        text_edit_buffer_.erase(lo, hi - lo);
        text_caret_ = lo;
        text_sel_anchor_ = lo;
    } else if (text_caret_ > 0) {
        const std::size_t prev = utf8_prev(text_edit_buffer_, text_caret_);
        text_edit_buffer_.erase(prev, text_caret_ - prev);
        text_caret_ = prev;
        text_sel_anchor_ = prev;
    } else {
        return;  // nothing to delete
    }
    set_editable_text_of(text_edit_target_, text_edit_buffer_);
    text_blink_ticks_ = 0;
}

void InspectorOverlay::text_delete_forward() {
    if (!text_editing()) return;
    if (!text_edit_target_reachable()) { clear_text_edit_state(); return; }
    if (text_has_selection()) {
        auto [lo, hi] = text_selection();
        text_edit_buffer_.erase(lo, hi - lo);
        text_caret_ = lo;
        text_sel_anchor_ = lo;
    } else if (text_caret_ < text_edit_buffer_.size()) {
        const std::size_t nxt = utf8_next(text_edit_buffer_, text_caret_);
        text_edit_buffer_.erase(text_caret_, nxt - text_caret_);
    } else {
        return;
    }
    set_editable_text_of(text_edit_target_, text_edit_buffer_);
    text_blink_ticks_ = 0;
}

bool InspectorOverlay::commit_text_edit() {
    if (!text_editing()) return false;
    // If the edit target was destroyed mid-edit (e.g. a
    // live React tree rebuild), do not deref it. Drop the edit state and bail.
    if (!text_edit_target_reachable()) {
        clear_text_edit_state();
        return false;
    }
    View* tgt = text_edit_target_;
    const std::string new_text = text_edit_buffer_;
    const std::string old_text = text_edit_original_;

    // Keep the live View text at the committed value.
    set_editable_text_of(tgt, new_text);

    bool emitted = false;
    const std::string anchor = tgt->anchor_id();
    const std::string path = text_tweak_path_;
    // The TWEAK persists only for anchored (imported) views, but the UNDO of
    // the live View text must work even for an UN-anchored view (e.g. a
    // --script Chainer label with no anchor_id) — otherwise Cmd+Z can't
    // restore edited text. So push the EditHistory entry
    // whenever the text actually changed + history is wired, and gate ONLY the
    // tweak apply/restore on the anchor.
    const bool anchored = (tweak_store_ != nullptr) && !anchor.empty();
    if (old_text != new_text) {
        std::optional<choc::value::Value> prior_val;
        if (anchored) {
            auto prior = tweak_store_->lookup(anchor, path);
            if (prior.has_value()) prior_val = *prior;
        }
        if (edit_history_) {
            auto* self = this;
            // For anchored targets the closures re-find the
            // live view by anchor at replay time (resolve_anchor → nullptr no-op
            // if a subtree rebuild freed it), so Cmd+Z after a live React rebuild
            // never derefs a dangling raw pointer. For UN-anchored targets there
            // is no anchor to re-find by, so we keep the raw `tgt` capture but
            // TRACK it; rebuild_flat_tree() clears the history if that view ever
            // leaves the tree, before these closures can run against freed memory.
            if (!anchored) track_raw_history_view(tgt);
            edit_history_->perform(
                [self, tgt, anchor, path, new_text, anchored]() {
                    View* live = anchored ? self->resolve_anchor(anchor) : tgt;
                    if (!live) return;  // freed/un-resolvable → graceful no-op
                    set_editable_text_of(live, new_text);
                    if (anchored && self->tweak_store_)
                        self->tweak_store_->apply_tweak(
                            anchor, path,
                            choc::value::createString(new_text),
                            "inspector-text-edit");
                },
                [self, tgt, anchor, path, old_text, prior_val, anchored]() {
                    View* live = anchored ? self->resolve_anchor(anchor) : tgt;
                    if (live) set_editable_text_of(live, old_text);
                    if (!(anchored && self->tweak_store_)) return;
                    if (prior_val.has_value())
                        self->tweak_store_->apply_tweak(
                            anchor, path, *prior_val, "inspector-undo");
                    else
                        self->tweak_store_->remove_tweak(anchor, path);
                },
                "edit-text");
            emitted = true;
        } else if (anchored) {
            tweak_store_->apply_tweak(anchor, path,
                                      choc::value::createString(new_text),
                                      "inspector-text-edit");
            emitted = true;
        }
    }

    text_edit_target_ = nullptr;
    text_edit_buffer_.clear();
    text_edit_original_.clear();
    text_caret_ = 0;
    text_sel_anchor_ = 0;
    return emitted;
}

void InspectorOverlay::cancel_text_edit() {
    if (!text_editing()) return;
    // If the target was destroyed mid-edit, drop the edit
    // state without touching freed memory (no revert to restore).
    if (!text_edit_target_reachable()) {
        clear_text_edit_state();
        return;
    }
    // Revert the live View to the original copy (live preview mutated it).
    set_editable_text_of(text_edit_target_, text_edit_original_);
    text_edit_target_ = nullptr;
    text_edit_buffer_.clear();
    text_edit_original_.clear();
    text_caret_ = 0;
    text_sel_anchor_ = 0;
}

bool InspectorOverlay::handle_text_input(const TextInputEvent& event) {
    // Send-to-agent free-text field: its own editing mode, independent of the
    // in-place text-editing path below. Insert typed characters at the caret.
    if (active_ && agent_field_active_) {
        if (!event.text.empty()) {
            agent_caret_ = std::min(agent_caret_, agent_text_buffer_.size());
            agent_text_buffer_.insert(agent_caret_, event.text);
            agent_caret_ += event.text.size();
        }
        return true;
    }
    if (!active_ || !text_editing()) return false;
    // Never write to a freed target. If the edited view
    // left the tree, drop the edit state and stop consuming text.
    if (!text_edit_target_reachable()) {
        clear_text_edit_state();
        return false;
    }
    if (event.text.empty()) return true;  // consume, nothing to append
    // Insert at the caret (replacing any selection) rather than
    // blindly appending, so typing in the middle / over a shift-selection
    // behaves like a real text field.
    text_insert(event.text);
    return true;
}

// ── Undo safety net helpers ─────────────────────────────────────────────────
//
// These capture the pre-gesture state at gesture start and restore it at
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
    // Capture transform-origin + overflow so undo of a
    // proportional resize reverts the top-left anchor and the box-clip.
    s.origin_x = v->transform_origin_x();
    s.origin_y = v->transform_origin_y();
    s.overflow = v->overflow();
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
    // Restore proportional-resize content scale.
    v->set_scale(s.scale);
    // Restore transform-origin + overflow so undo of a
    // proportional resize removes the top-left anchor + box-clip we applied.
    v->set_transform_origin(s.origin_x, s.origin_y);
    v->set_overflow(s.overflow);
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

// ── Drift detection ─────────────────────────────────────────────────────────
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

// ── Reconciliation tab ─────────────────────────────────────────────────────
//
// Classifies every stored tweak into one of three reconciliation
// states (locked-to-source / drifted / unresolvable). The classifier
// is purely a *read* over the existing TweakStore + live view tree —
// it never mutates either, never spawns a process, and never throws.
// It deliberately reuses the drift machinery's "anchor present in the
// live tree" notion (TweakStore::DriftReason::anchor_not_found) so the
// reconciliation tab and the drift drawer agree on what counts
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

// Product of this view's scale and all ancestor scales
// up to root_ (exclusive). Mirrors the cumulative `scale(s,s)` View::paint_all
// applies down the subtree, so the inline-text-edit overlay can map unscaled
// element-local caret offsets onto the rendered (scaled) glyphs.
float InspectorOverlay::effective_scale_in_root(const View* v) const {
    float s = 1.0f;
    const View* cur = v;
    while (cur && cur != &root_) {
        s *= cur->scale();
        cur = cur->parent();
    }
    return s;
}

// ── Drag handle hit-test ───────────────────────────────────────────────────
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
    const float midx = r.x + r.width * 0.5f;
    const float midy = r.y + r.height * 0.5f;
    // Corners win over edges (they sit at the same coordinates as the
    // ends of two edges; a corner press should resize both axes).
    if (in_box(r.x,             r.y))              return DragCorner::nw;
    if (in_box(r.x + r.width,   r.y))              return DragCorner::ne;
    if (in_box(r.x,             r.y + r.height))   return DragCorner::sw;
    if (in_box(r.x + r.width,   r.y + r.height))   return DragCorner::se;
    // Edge midpoint handles are single-axis resize targets.
    if (in_box(midx,            r.y))              return DragCorner::n;
    if (in_box(midx,            r.y + r.height))   return DragCorner::s;
    if (in_box(r.x + r.width,   midy))             return DragCorner::e;
    if (in_box(r.x,             midy))             return DragCorner::w;
    return DragCorner::none;
}

// ── Drag-to-move helpers ───────────────────────────────────────────────────

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

// ── Cursor affordances over the selected element ───────────────────────────
// Continuous hover feedback so the user SEES move-vs-resize before pressing.
// Mirrors the gesture hit-test order: a corner handle wins (diagonal resize
// cursor), then the body (move cursor), else no override. During an ACTIVE
// gesture the cursor is pinned to that gesture (resize while resizing, move
// while moving) so it doesn't flicker as the pointer drifts off the original
// target mid-drag.
InspectorOverlay::CursorAffordance
InspectorOverlay::cursor_affordance_at(Point pos) const {
    if (!selected_ || !dragging_enabled_) return CursorAffordance::none;

    // Active resize: pin to the dragged handle's affordance.
    if (active_drag_ != DragCorner::none)
        return affordance_for_corner(active_drag_);
    // Active move: pin to the move cursor regardless of pointer drift.
    if (move_active_) return CursorAffordance::move;

    // Idle hover: handle → resize; body → move; outside → none.
    auto handle = hit_test_drag_handle(pos);
    if (handle != DragCorner::none) return affordance_for_corner(handle);
    if (hit_test_body(pos)) return CursorAffordance::move;
    return CursorAffordance::none;
}

InspectorOverlay::CursorAffordance
InspectorOverlay::affordance_for_corner(DragCorner c) {
    switch (c) {
        case DragCorner::nw:
        case DragCorner::se: return CursorAffordance::resize_nw_se;
        case DragCorner::ne:
        case DragCorner::sw: return CursorAffordance::resize_ne_sw;
        case DragCorner::n:
        case DragCorner::s:  return CursorAffordance::resize_ns;
        case DragCorner::e:
        case DragCorner::w:  return CursorAffordance::resize_ew;
        case DragCorner::none: break;
    }
    return CursorAffordance::none;
}

int InspectorOverlay::cursor_style_for(Point pos) const {
    switch (cursor_affordance_at(pos)) {
        case CursorAffordance::move:
            // 4-way move cursor; macOS maps multi_directional_resize to a
            // hand, which reads as "grab to move".
            return static_cast<int>(View::CursorStyle::multi_directional_resize);
        case CursorAffordance::resize_nw_se:
            return static_cast<int>(View::CursorStyle::top_left_resize);
        case CursorAffordance::resize_ne_sw:
            return static_cast<int>(View::CursorStyle::top_right_resize);
        case CursorAffordance::resize_ns:
            return static_cast<int>(View::CursorStyle::vertical_resize);
        case CursorAffordance::resize_ew:
            return static_cast<int>(View::CursorStyle::horizontal_resize);
        case CursorAffordance::none:
            return -1;  // defer to the normal hit-view cursor
    }
    return -1;
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
    // AbsoluteLayout.cpp:209-224 and Yoga's border-edge formula).
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

// ── Reflow-aware move (drop-target resolution + commit) ────────────────────

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

    // The dragged element's current parent distinguishes a same-parent REORDER
    // from a cross-parent REPARENT (drop INSIDE).
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

    // Build the paint affordance. The choice of INSERTION LINE vs container
    // HIGHLIGHT is driven by whether the cursor resolves to a position
    // BETWEEN/around the container's existing children, NOT by whether the
    // drop reparents:
    //  - Container has visible sibling slots -> crisp blue insertion LINE at
    //    the resolved boundary (before-first / between-each / after-last).
    //    This is the Figma-style "it'll drop here" line, and it shows for
    //    BOTH a same-parent reorder AND a cross-parent reparent into a
    //    populated container, so the user almost always sees a precise drop
    //    position rather than a vague highlight.
    //  - Container has NO sibling slots (empty interior, nothing to insert
    //    between) -> translucent container HIGHLIGHT (drop-inside). This is
    //    the only case where a boundary line would be meaningless.
    // The line spans the container's full cross-axis extent so it reads as a
    // 2px boundary across the whole row/column, and it tracks the cursor on
    // every drag tick because resolve_drop_target() runs per move event.
    const Rect cr = view_bounds_in_root(container);
    if (!slots.empty()) {
        // Insertion line at the boundary for index `idx`:
        //   idx == 0    -> before the first sibling
        //   0 < idx < N -> between slots[idx-1] and slots[idx] (in the gap)
        //   idx == N    -> after the last sibling
        drop_indicator_is_line_ = true;
        if (horizontal) {
            float line_x;
            if (idx <= 0) {
                line_x = slots.front().bounds.x;
            } else if (idx >= static_cast<int>(slots.size())) {
                line_x = slots.back().bounds.x + slots.back().bounds.width;
            } else {
                const float prev_edge =
                    slots[idx - 1].bounds.x + slots[idx - 1].bounds.width;
                const float next_edge = slots[idx].bounds.x;
                line_x = 0.5f * (prev_edge + next_edge);
            }
            drop_indicator_ = {line_x - 1.0f, cr.y, 2.0f, cr.height};
        } else {
            float line_y;
            if (idx <= 0) {
                line_y = slots.front().bounds.y;
            } else if (idx >= static_cast<int>(slots.size())) {
                line_y = slots.back().bounds.y + slots.back().bounds.height;
            } else {
                const float prev_edge =
                    slots[idx - 1].bounds.y + slots[idx - 1].bounds.height;
                const float next_edge = slots[idx].bounds.y;
                line_y = 0.5f * (prev_edge + next_edge);
            }
            drop_indicator_ = {cr.x, line_y - 1.0f, cr.width, 2.0f};
        }
    } else {
        // Empty container interior -- no sibling boundary to draw a line at,
        // so fall back to the drop-inside container highlight.
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

    // Capture each child's old order so we can persist a `layout.order` tweak
    // for every node whose order actually changed, not just the dragged one.
    // The tweak/lock path round-trips `layout.order` into the generated source
    // as `el.style.order`.
    auto record_old_order = [](View* v) { return std::pair<View*, int>{v, v->flex().order}; };
    std::vector<std::pair<View*, int>> old_orders;
    old_orders.reserve(others.size() + 1);
    old_orders.push_back(record_old_order(dragged));
    for (View* o : others) old_orders.push_back(record_old_order(o));

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

    // Persist `layout.order` for every anchored child whose order changed.
    // Keyed by the child's own anchor (NOT selected_), so a normalized sibling
    // is locked too. Un-anchored children can't be locked to source — skipped.
    if (tweak_store_) {
        for (const auto& [v, old_order] : old_orders) {
            const int new_order = v->flex().order;
            if (new_order == old_order) continue;
            const std::string& a = v->anchor_id();
            if (a.empty()) continue;
            tweak_store_->apply_tweak(
                a, "layout.order",
                choc::value::createInt64(static_cast<int64_t>(new_order)),
                "inspector-reflow-order");
        }
    }

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
    // text_edit_target_ is a raw View* set during an inline
    // edit. If the edited Label/TextEditor was destroyed during this rebuild
    // (e.g. a live React tree rebuild), clear the WHOLE text-edit state without
    // touching the freed view, so the next handle_text_input / Backspace /
    // commit / cancel does not deref freed memory. Pointer-compare only —
    // in_tree() never dereferences the target.
    if (text_edit_target_ && !in_tree(text_edit_target_))
        clear_text_edit_state();

    // Un-anchored EditHistory fallback. Anchored undo/redo
    // closures self-heal via resolve_anchor() at replay time, but closures that
    // captured a raw View* for an UN-anchored target (no anchor to re-find it
    // by) would deref freed memory if that view left the tree in this rebuild.
    // clear_edit_history() only fires at ROOT replacement; a live SUBTREE
    // rebuild does not trigger it. So if any tracked raw view is now gone, drop
    // the WHOLE history here (before the freed views' closures can run) and the
    // tracking list with it. Anchored captures are never tracked, so this is a
    // no-op on the common path. Pointer-compare only — never derefs a freed view.
    if (any_raw_history_view_dangling()) {
        if (edit_history_) edit_history_->clear();
        raw_history_views_.clear();
    }
}

// ── Input handling ──────────────────────────────────────────────────────────

bool InspectorOverlay::handle_key_event(const KeyEvent& event) {
    // Cmd+I (macOS) / Ctrl+I (Windows/Linux) toggles inspector
    if (event.key == KeyCode::i && event.isMainModifier() && event.is_down) {
        toggle();
        return true;
    }

    // Cmd+Z undo / Cmd+Shift+Z (or Cmd+Y) redo,
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

    // Send-to-agent text field owns the keyboard while focused: Enter submits
    // the request through the sink, Esc defocuses (keeping the draft), Backspace
    // trims one codepoint, Left/Right move the caret. Character input arrives via
    // handle_text_input(). Checked before the inline text-edit / numeric paths so
    // the field's control keys are exclusive while it is active.
    if (active_ && agent_field_active_ && event.is_down) {
        if (event.key == KeyCode::enter) {
            submit_agent_request();
            return true;
        }
        if (event.key == KeyCode::escape) {
            agent_field_active_ = false;
            return true;
        }
        if (event.key == KeyCode::backspace) {
            if (agent_caret_ > 0 && !agent_text_buffer_.empty()) {
                // Trim one UTF-8 codepoint ending at the caret (skip continuation
                // bytes 0b10xxxxxx) so a multibyte glyph deletes whole.
                std::size_t n = 1;
                while (agent_caret_ - n > 0 &&
                       (static_cast<unsigned char>(agent_text_buffer_[agent_caret_ - n]) & 0xC0) ==
                           0x80)
                    ++n;
                n = std::min(n, agent_caret_);
                agent_text_buffer_.erase(agent_caret_ - n, n);
                agent_caret_ -= n;
            }
            return true;
        }
        if (event.key == KeyCode::left) {
            if (agent_caret_ > 0) --agent_caret_;
            return true;
        }
        if (event.key == KeyCode::right) {
            if (agent_caret_ < agent_text_buffer_.size()) ++agent_caret_;
            return true;
        }
    }

    // Inline text editing (Text tool) owns the keyboard while a
    // text element's copy is being edited. Enter commits (live text +
    // a `text` tweak, one undoable unit), Esc cancels (restores the
    // original copy), Backspace trims the last UTF-8 codepoint. The
    // actual character input arrives via handle_text_input() (KeyEvent
    // carries no character payload); this branch handles only the
    // control keys. Checked BEFORE the numeric field-edit + Esc paths
    // below so the Text tool's edit owns those keys exclusively.
    if (active_ && text_editing() && event.is_down) {
        if (event.key == KeyCode::enter) {
            commit_text_edit();
            return true;
        }
        if (event.key == KeyCode::escape) {
            cancel_text_edit();
            // Figma parity: Esc out of text editing returns to the Select
            // (move/resize) tool — selection goes blue-edit → orange-select.
            set_tool(Tool::select);
            return true;
        }
        // Guard against a target freed mid-edit for every
        // mutating key path below.
        if (event.key == KeyCode::backspace) {
            if (!text_edit_target_reachable()) {
                clear_text_edit_state();
                return true;
            }
            text_delete_backward();  // selection-aware delete
            return true;
        }
        if (event.key == KeyCode::delete_) {
            if (!text_edit_target_reachable()) {
                clear_text_edit_state();
                return true;
            }
            text_delete_forward();
            return true;
        }
        // Cmd/Ctrl clipboard + select-all. Bound BEFORE the
        // return-false char passthrough so Cmd+A/C/V/X don't fall through to
        // insertText (which would type a literal 'a'/'c'/…). isMainModifier()
        // is Cmd on macOS, Ctrl elsewhere.
        if (event.isMainModifier()) {
            switch (event.key) {
                case KeyCode::a: text_select_all(); return true;
                case KeyCode::c: text_copy();       return true;
                case KeyCode::x: text_cut();        return true;
                case KeyCode::v: text_paste();      return true;
                // Cmd+Left / Cmd+Right = line start / end (single-line).
                case KeyCode::left:  text_move_home(event.isShiftDown()); return true;
                case KeyCode::right: text_move_end(event.isShiftDown());  return true;
                default: break;
            }
        }
        // Caret movement. Shift extends the selection; the
        // Option/Alt modifier jumps by word. Up/Down map to start/end in
        // this single-line in-place edit.
        if (event.key == KeyCode::left || event.key == KeyCode::right) {
            const int dir = (event.key == KeyCode::left) ? -1 : 1;
            if (event.isAltDown()) {
                text_move_word(dir, event.isShiftDown());
            } else {
                text_move_caret(dir, event.isShiftDown());
            }
            return true;
        }
        if (event.key == KeyCode::home || event.key == KeyCode::up) {
            text_move_home(event.isShiftDown());
            return true;
        }
        if (event.key == KeyCode::end_ || event.key == KeyCode::down) {
            text_move_end(event.isShiftDown());
            return true;
        }
        // Return FALSE for all other key-downs (do NOT swallow) so the mac
        // host proceeds to interpretKeyEvents: -> insertText: -> the text
        // hook -> handle_text_input(), which is the ONLY path that delivers
        // the actual typed character. The control keys (Enter/Esc/Backspace/
        // Delete), clipboard combos, and caret navigation were consumed above;
        // everything else is character input. A tool/toggle flip can't fire
        // from here because this block returns before the V/T/D/P handlers.
        return false;
    }

    // Field-edit mode owns the keyboard while a numeric
    // value is being edited. Esc cancels, Enter commits, Tab walks
    // to the next field; arrows nudge; digits/sign/decimal extend
    // the buffer. The plain Esc-deselect path below only fires when no edit is
    // in progress (cancel_field_edit() leaves the inspector active so the user
    // can keep poking around).
    if (active_ && !editing_field_.empty() && event.is_down) {
        if (handle_edit_key(event)) return true;
    }

    // Esc deselects in one press and keeps inspect mode active. Edit mode
    // already consumed Esc above to cancel the edit, so this never fires
    // mid-edit.
    if (active_ && event.key == KeyCode::escape && event.is_down) {
        // Esc = deselect in one press, and stay in inspect mode so hover +
        // click keep working without a Cmd+I cycle.
        if (selected_) {
            // Cancel any in-progress field edit first so a stale
            // editing_field_ doesn't keep swallowing the keyboard /
            // pinning selection after the deselect.
            // Deselect via the same full set_active(false) -> set_active(true)
            // cycle as Cmd+I so all transient state (editable_fields_,
            // eyedropper_active_, zoom_active_, hovered_, etc.) is cleared and
            // hover + click immediately work again. active_ ends true; the
            // floating inspector window is unaffected because the host owns it.
            set_active(false);
            set_active(true);
        }
        // Esc never exits the inspector (use Cmd+I / the window close button
        // for that). A second Esc with nothing selected is a no-op so the
        // overlay stays active and hover + click keep working.
        return true;
    }

    // D toggles drag-handles mode (no modifier; only when
    // the inspector is active so a hotkey collision in a plain text
    // input doesn't accidentally flip drag mode).
    if (active_ && event.key == KeyCode::d && event.is_down &&
        event.modifiers == 0) {
        toggle_dragging();
        return true;
    }

    // P toggles the per-pass attribution viewer (no
    // modifier; only while active, same rationale as the D toggle).
    if (active_ && event.key == KeyCode::p && event.is_down &&
        event.modifiers == 0) {
        toggle_pass_viewer();
        return true;
    }

    // Figma-style tool palette. V selects the Select tool, T the
    // Text tool (Figma convention). Both no-modifier, inspector-active,
    // and gated behind not-mid-edit (numeric field edit OR inline text
    // edit) so a V/T typed into an edit buffer never flips the tool.
    // Bound BEFORE the Shift+T tweak-panel toggle below so the bare keys
    // land here first.
    if (active_ && editing_field_.empty() && !text_editing() &&
        event.is_down && event.modifiers == 0) {
        if (event.key == KeyCode::v) {
            set_tool(Tool::select);
            return true;
        }
        if (event.key == KeyCode::t) {
            set_tool(Tool::text);
            return true;
        }
    }

    // Shift+T toggles the tweak management panel. The bare
    // `T` key selects the Text tool, so the tweak-management
    // panel moved to Shift+T (a free, non-conflicting trigger). Guarded
    // behind not-editing so a Shift+T while editing doesn't flip it.
    if (active_ && editing_field_.empty() && !text_editing() &&
        event.key == KeyCode::t && event.is_down &&
        event.modifiers == kModShift) {
        toggle_tweaks_panel();
        return true;
    }

    // J jumps to the selected view's authored JSX source
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

    // E toggles eyedropper mode (no modifier; same opt-in
    // discipline as the D-key drag toggle above). Entering edit mode
    // already swallows keys before this point, so the E-key can't
    // collide with numeric-field editing.
    if (active_ && event.key == KeyCode::e && event.is_down &&
        event.modifiers == 0) {
        toggle_eyedropper();
        return true;
    }

    // Z toggles the 20× zoom loupe (no modifier; only when
    // the inspector is active, same guard as the D-key path). D/E/T/Shift+T
    // are already claimed by drag / eyedropper / text / tweak panel, so Z is
    // the natural free letter for "zoom".
    if (active_ && event.key == KeyCode::z && event.is_down &&
        event.modifiers == 0) {
        toggle_zoom();
        return true;
    }

    // R toggles the reconciliation tab (no modifier; only
    // while the inspector is active, same opt-in discipline as the D /
    // E / P / Z toggles). Guarded behind not-editing so typing an 'r'
    // into a field-edit buffer can't flip the tab. D/E/T/J/P/Z are
    // already claimed, so R ("reconcile") is the natural free letter.
    if (active_ && editing_field_.empty() && event.key == KeyCode::r &&
        event.is_down && event.modifiers == 0) {
        toggle_reconcile_tab();
        return true;
    }

    // A toggles the texture-atlas viewer tab (no modifier;
    // only while the inspector is active, same opt-in discipline as the
    // D / E / P / Z / R toggles). Guarded behind not-editing so typing
    // an 'a' into a field-edit buffer can't flip the tab. D/E/T/J/P/Z/R
    // are already claimed, so A ("atlas") is the natural free letter.
    if (active_ && editing_field_.empty() && event.key == KeyCode::a &&
        event.is_down && event.modifiers == 0) {
        toggle_atlas_viewer();
        return true;
    }

    // M toggles the selection mode between follows_focus
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

    // ── Gesture-phase resolution ──────────────────────────────────
    // The move/resize gesture machines below need to distinguish a
    // PRESS (begin), a DRAG TICK (live update), and a RELEASE (commit).
    // Two callers feed events with OPPOSITE is_down conventions,
    // because they disagree about what is_down MEANS:
    //   * headless tests, and any caller for which is_down means "the
    //     button state CHANGED": press=is_down, drag=!is_down,
    //     release=is_down. No explicit phase is set.
    //   * the mac platform host, for which is_down means "the button is
    //     currently HELD": press=down, drag=down, release=up. It also
    //     sets MouseEvent::phase explicitly.
    // Trust the explicit phase when present; otherwise fall back to the
    // state-CHANGED inference so those callers are unchanged.
    //
    // `gesture_active` is whether a move OR resize is already in flight;
    // it decides how the legacy is_down value is read for THIS event.
    const bool gesture_active = (active_drag_ != DragCorner::none) ||
                                move_active_;
    bool is_press, is_drag_tick, is_release;
    if (event.hasExplicitPhase()) {
        is_press = event.isPress();
        is_drag_tick = event.isDrag();
        is_release = event.isRelease();
    } else if (gesture_active) {
        // Legacy convention DURING a gesture: is_down=false is a drag
        // tick, is_down=true is the release.
        is_press = false;
        is_drag_tick = !event.is_down;
        is_release = event.is_down;
    } else {
        // No gesture in flight: a button-down may BEGIN one.
        is_press = event.is_down;
        is_drag_tick = false;
        is_release = false;
    }

    // ── Text tool: click a text element to edit its copy ───────────
    //
    // In Text-tool mode a canvas press on a text-bearing View (Label /
    // TextEditor) begins an inline copy edit of THAT element instead of
    // selecting / moving / resizing it. Runs BEFORE the move/resize
    // gesture machines and the selection hit-test so the press never
    // starts a drag. Panel clicks (tree / props / tweak rows) still fall
    // through to the panel path so the user keeps inspector navigation.
    // A press that misses any text element is consumed as a no-op so it
    // doesn't accidentally move/select while the Text tool is active —
    // matching Figma, where the Text tool never drag-moves shapes.
    if (tool_ == Tool::text && is_press && !point_in_panel(pos)) {
        // Commit any in-progress text edit first (click-away-to-commit),
        // unless the press lands back on the same element being edited.
        const View* hit = root_.hit_test(pos);
        const View* text_host = hit;
        while (text_host && !view_has_editable_text(text_host))
            text_host = text_host->parent();
        if (text_editing() && text_host != text_edit_target_) {
            commit_text_edit();
        }
        if (text_host) {
            begin_text_edit(const_cast<View*>(text_host));
        }
        return true;  // Text tool owns canvas presses (no drag/select)
    }

    // The loupe re-centers on the cursor for every mouse
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

    // ── Eyedropper mode ────────────────────────────────────────────
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
        if (is_press) {
            // Resolved-style sampling is synchronous + frame-
            // independent, so a click without a prior move still
            // picks a real color (covers headless / scripted use).
            //
            // The click must be authoritative on the click coordinate.
            // Invalidate any prior sample FIRST — a
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

    // ── Drag-handle gesture state machine ─────────────────────────
    // Under the state-CHANGED convention above, is_down=true arrives
    // ONLY for the initial press; subsequent moves AND the release
    // both arrive as is_down=false. Without a distinct
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
        if (is_release) {
            // Release: end the drag. Don't consume — let this click
            // fall through to normal selection logic so the user can
            // immediately re-target without a wasted click.
            active_drag_ = DragCorner::none;

            // Commit the completed resize as ONE undoable unit. The
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
        } else if (is_drag_tick) {
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
                // Edge handles resize a single axis.
                case DragCorner::n:  new_h -= dy; break;
                case DragCorner::s:  new_h += dy; break;
                case DragCorner::e:  new_w += dx; break;
                case DragCorner::w:  new_w -= dx; break;
                case DragCorner::none: break;
            }
            // Floor at 4px so the view never collapses small enough
            // that handles overlap and the user can't grab them.
            new_w = std::max(4.0f, new_w);
            new_h = std::max(4.0f, new_h);

            if (resize_proportional_) {
                // Proportional resize (Shift). Instead of stretching
                // the box, SCALE the container's content uniformly so the
                // panel keeps its internal proportions. The box itself still
                // grows (preferred_*) so the scaled content has room; the
                // uniform set_scale() keeps children's relative layout.
                //
                // Keep scaled content inside the resize box. Two coupled
                // constraints enforce that:
                //   1. Anchor the scale to the box's TOP-LEFT corner
                //      (transform-origin 0,0) instead of the default center
                //      (0.5,0.5). With a center origin the content scales
                //      symmetrically about the middle and the top/left edges
                //      grow OUT of the box. A top-left origin
                //      makes content grow DOWN-RIGHT into the box, matching
                //      the fixed top-left of drag_start_bounds_.
                //   2. Pick the scale ratio from the SMALLER axis
                //      (min, not average). Content that fit the start box
                //      (w0 x h0) scaled by min(new_w/w0, new_h/h0) is
                //      guaranteed <= the new box on BOTH axes, so it can never
                //      exceed the box even on a non-uniform corner drag.
                // Belt-and-suspenders: also clip the container to its bounds
                // (overflow:hidden) so any residual sub-pixel spill from
                // descendant transforms is contained by the box rectangle.
                const float w0 = std::max(1.0f, drag_start_bounds_.width);
                const float h0 = std::max(1.0f, drag_start_bounds_.height);
                const float ratio_w = new_w / w0;
                const float ratio_h = new_h / h0;
                // Uniform scale bounded by the tighter axis so the scaled
                // content stays within the box on both axes, then clamp.
                float ratio = std::min(ratio_w, ratio_h);
                float new_scale = std::clamp(drag_start_scale_ * ratio,
                                             0.1f, 10.0f);
                // Top-left transform-origin: content grows into the box from
                // its top-left corner, never past it.
                selected_->set_transform_origin(0.0f, 0.0f);
                selected_->set_scale(new_scale);
                // Clip scaled content to the box so nothing spills outside the
                // selection rectangle.
                selected_->set_overflow(View::Overflow::hidden);
                // Grow the box to match so the scaled content has room.
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

    // Hand-off from selection to drag: if drag-handles
    // mode is enabled, a view is selected, and the press lands on a
    // drag handle, START the resize and consume. Handle-press wins over
    // body-press (move) and over selection hit-testing: the resize-start check
    // is BEFORE the move-start
    // check below, which is BEFORE the canvas select path.
    if (is_press && active_drag_ == DragCorner::none && !move_active_ &&
        selected_) {
        auto handle = hit_test_drag_handle(pos);
        if (handle != DragCorner::none) {
            active_drag_ = handle;
            drag_start_pos_ = pos;
            drag_start_bounds_ = view_bounds_in_root(selected_);
            drag_start_pref_w_ = selected_->flex().preferred_width;
            drag_start_pref_h_ = selected_->flex().preferred_height;
            // Shift at press chooses proportional resize (scale the
            // container's content) over plain box resize (reflow children).
            resize_proportional_ = event.isShiftDown();
            drag_start_scale_ = selected_->scale();
            // Capture the pre-resize View inputs + prior tweak values
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

    // ── Drag-to-move gesture state machine ─────────────────────────
    // Same press/move/release convention as the resize machine: while a
    // move is active, is_down=false events live-update + overwrite the
    // tweak batch, the next is_down=true ends the gesture. Runs BEFORE
    // the panel-area test so a move started on the canvas stays owned by
    // this branch even if the cursor briefly enters the panel.
    if (move_active_ && selected_) {
        if (is_release) {
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
                // Capture the structural anchors BEFORE the commit
                // so the source-rewrite sink can lock the reparent into the
                // generated artifact (and undo can re-derive the inverse). Empty
                // when a view carries no design provenance — the sink only fires
                // for fully-anchored reparents.
                const std::string child_anchor = tgt->anchor_id();
                const std::string old_parent_anchor =
                    before_parent ? before_parent->anchor_id() : std::string{};
                // Capture the BEFORE insertion slot (the
                // anchor of the visible sibling tgt followed under its OLD
                // parent, in flex-order) so undo can re-derive the inverse
                // source rewrite at the right position, not always first-child.
                const std::string old_insert_after_anchor =
                    preceding_sibling_anchor(before_parent, tgt);
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
                    const std::string new_parent_anchor =
                        after_parent ? after_parent->anchor_id() : std::string{};
                    // Capture the AFTER insertion slot: the anchor of
                    // the visible sibling tgt now follows under the NEW parent
                    // (flex-order). Threaded into ReparentSourceEdit so the
                    // source rewrite drops the moved block at the dragged
                    // position instead of always as the parent's first child.
                    const std::string new_insert_after_anchor =
                        preceding_sibling_anchor(after_parent, tgt);
                    // Emit a structural source rewrite ONLY for a
                    // genuine cross-parent reparent (the same-parent reorder is
                    // persisted as a layout.order tweak in commit_reflow_drop)
                    // where every anchor resolves and a sink is wired. The do_fn
                    // locks the
                    // child under the NEW parent; the undo_fn re-emits the edit
                    // with the OLD parent so the host re-derives the inverse
                    // source rewrite. The engine's idempotent `already_current`
                    // path keeps repeated emits byte-stable.
                    const bool emit_source_reparent =
                        has_reparent_source_sink() &&
                        after_parent != before_parent &&
                        !child_anchor.empty() &&
                        !new_parent_anchor.empty() &&
                        !old_parent_anchor.empty();
                    auto* self = this;
                    // The reparent undo/redo closures must not
                    // capture raw View* that dangle after a live React SUBTREE
                    // rebuild (clear_edit_history only fires at ROOT replacement).
                    // Resolve each participant by its stable anchor at replay time
                    // when anchored; fall back to the raw pointer (TRACKED, so
                    // rebuild_flat_tree clears the history if it leaves the tree)
                    // when a participant carries no anchor. A child that can't be
                    // resolved → graceful no-op.
                    const bool child_anchored = !child_anchor.empty();
                    const bool new_parent_anchored = !new_parent_anchor.empty();
                    const bool old_parent_anchored = !old_parent_anchor.empty();
                    if (!child_anchored) track_raw_history_view(tgt);
                    if (!new_parent_anchored) track_raw_history_view(after_parent);
                    if (!old_parent_anchored) track_raw_history_view(before_parent);
                    // do_fn re-applies the AFTER tree state; undo_fn restores
                    // BEFORE. reparent_to is the structural primitive (no-op
                    // when parent unchanged), order is rewritten directly.
                    edit_history_->perform(
                        [self, tgt, after_parent, after_index, after_order,
                         emit_source_reparent, child_anchor, new_parent_anchor,
                         new_insert_after_anchor,
                         child_anchored, new_parent_anchored]() {
                            View* child = child_anchored
                                ? self->resolve_anchor(child_anchor) : tgt;
                            if (!child) return;  // freed → graceful no-op
                            View* np = new_parent_anchored
                                ? self->resolve_anchor(new_parent_anchor)
                                : after_parent;
                            self->reparent_view(child, np, after_index);
                            child->flex().order = after_order;
                            if (np) np->invalidate_layout();
                            child->invalidate_layout();
                            if (emit_source_reparent && self->reparent_source_sink_)
                                self->reparent_source_sink_(
                                    {child_anchor, new_parent_anchor,
                                     new_insert_after_anchor});
                        },
                        [self, tgt, before_parent, before_index, before_order,
                         emit_source_reparent, child_anchor, old_parent_anchor,
                         old_insert_after_anchor,
                         child_anchored, old_parent_anchored]() {
                            View* child = child_anchored
                                ? self->resolve_anchor(child_anchor) : tgt;
                            if (!child) return;  // freed → graceful no-op
                            View* op = old_parent_anchored
                                ? self->resolve_anchor(old_parent_anchor)
                                : before_parent;
                            self->reparent_view(child, op, before_index);
                            child->flex().order = before_order;
                            if (op) op->invalidate_layout();
                            child->invalidate_layout();
                            if (emit_source_reparent && self->reparent_source_sink_)
                                self->reparent_source_sink_(
                                    {child_anchor, old_parent_anchor,
                                     old_insert_after_anchor});
                        },
                        "move-reflow");
                }
            }
            // Clear drop affordance + drag ghost after a completed gesture.
            drop_target_ = nullptr;
            drop_indicator_ = {};
            move_ghost_ = {};
            // fall through to the normal handlers below
        } else if (is_drag_tick && move_float_) {
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

            // Emit the three move tweaks as ONE atomic batch:
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
        } else if (is_drag_tick) {
            // ── plain move tick: REFLOW-AWARE drop-target resolution ───
            // We don't mutate the layout while dragging — instead we resolve
            // a drop target and update the paint affordance (insertion line /
            // container highlight). The actual reorder/reparent happens on
            // release (commit_reflow_drop). This keeps the dragged element
            // visually in place and the rest of the tree stable until commit.
            // Track the cursor for the follow-ghost so the drag reads
            // as smooth motion (the ghost is pure paint — no relayout here, so
            // the drop preview never flickers from re-layout churn).
            move_cursor_ = pos;
            resolve_drop_target(pos);
            return true;  // consume the move event
        }
    }

    // Hand-off from selection to move: if dragging mode is enabled,
    // a view is selected, and the press lands on the view's BODY (not a
    // handle), START a move of THAT element. Guard grid children: a
    // direct child of a grid container ignores position/top/left, so
    // refuse with a clear affordance instead of a silently-broken drag.
    //
    // This body-press-of-selected → move check must run before the canvas
    // selection hit-test below. hit_test_body
    // is scoped to the CURRENTLY-selected view's bounds, so a press on
    // the selected element begins a move of it (capturing a stable grab
    // offset) and consumes the event — it never re-runs selection
    // hit-testing nor grows a selection rectangle. The drag ticks then
    // route to the active-move branch above (move_active_), not here.
    if (is_press && active_drag_ == DragCorner::none && !move_active_ &&
        selected_ && hit_test_body(pos)) {
        // The modifier picks the move mode:
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
        // Capture a stable grab offset + the element's size at press
        // so the reflow-move ghost can follow the cursor smoothly (no teleport
        // on grab, constant ghost size, no per-tick relayout).
        {
            Rect sb = view_bounds_in_root(selected_);
            move_grab_offset_ = {pos.x - sb.x, pos.y - sb.y};
            move_ghost_ = sb;
            move_cursor_ = pos;
        }
        // Capture pre-move View inputs (position + insets + bounds)
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
        // Clear Alt-hover state before panel-entry early-return. Without this,
        // moving from an
        // Alt-hovered view straight into the inspector panel leaves
        // `alt_hover_target_` pointing at the previous view and the
        // overlay keeps drawing the live distance line even though
        // the cursor has left the view area.
        alt_hover_target_ = nullptr;

        if (is_press) {
            // Clicks on the semantic-knob controls / send-to-agent field at the
            // top of the tweaks panel. Checked first so a knob or field click
            // never falls through to a tweak-row icon or tree selection. The hit
            // lists (knob_hits_, agent field rects) are populated by the most
            // recent paint_knob_controls() call. A non-consuming click here also
            // defocuses the text field.
            if (tweaks_panel_visible_ && handle_knob_panel_click(pos))
                return true;

            // Clicks on a tweak-row icon (bypass / lock /
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
                            // Capture the value being deleted FIRST so
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

            // A click on the drift-drawer header toggles the
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

            // Clicks on numeric values in the property
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
    if (is_press && !editing_field_.empty()) {
        commit_field_edit();
    }

    // Mouse in view area — pick view under cursor for highlighting
    auto* hit = root_.hit_test(pos);
    if (hit) {
        hovered_ = hit;

        // Selection-mode toggle. In follows_mouse mode the
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
        // A pure hover-move only: neither press, drag tick, nor release, so
        // an explicit-phase drag begun on empty canvas never re-selects
        // mid-drag. In the state-CHANGED convention this reduces to the old
        // !event.is_down test for a fresh (no active gesture) event.
        if (selection_mode_ == SelectionMode::follows_mouse &&
            !is_press && !is_drag_tick && !is_release && !event.isAltDown() &&
            !is_editing()) {
            selected_ = hit;
        }
    }

    // Alt-hover sibling distance (Figma-style). Tracks the
    // hovered View as an alt_hover_target_ whenever Alt is held AND a
    // selected_ exists; clears as soon as Alt is released. The dynamic
    // line paints from selected_ to alt_hover_target_ in
    // paint_distance_lines().
    if (event.isAltDown() && selected_ && hit && hit != selected_) {
        alt_hover_target_ = hit;
    } else {
        alt_hover_target_ = nullptr;
    }

    // Only a genuine press selects. A drag tick that reaches
    // here (an explicit-phase drag begun on empty canvas where no move /
    // resize gesture engaged) carries is_down=true but is_press=false, so
    // it must NOT re-run selection hit-testing every tick; otherwise selection
    // would jump to another object mid-drag. It is still consumed below so it
    // never leaks to a widget. In the legacy
    // is_down convention is_press == event.is_down for a fresh (no active
    // gesture) event, so click-to-select is unchanged.
    if (is_press) {
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
    // Consume explicit-phase drag/release events over the canvas so they
    // never leak through to a widget, but without mutating selection.
    if (event.hasExplicitPhase() && (is_drag_tick || is_release))
        return true;

    // Hover events: don't consume — let normal hover effects work
    return false;
}

bool InspectorOverlay::point_in_panel(Point p) const {
    // In the minimal manipulate layer there is no dev side-panel, so
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

// All paint_* methods (paint, paint_highlight, paint_box_model, the in-canvas
// text-edit overlay + caret/selection shaping, the move/resize drop
// indicator + drag ghost, panel/tree/props/stats/tweaks sections, the
// reconcile/atlas tabs and drift drawer) plus the paint-only tweak_action_at
// live in inspector_overlay_paint.cpp.

// The live-editable box-model fields (editable_field_at /
// read_field_value / write_field_value / begin_field_edit /
// commit_field_edit / cancel_field_edit / apply_edit_buffer_to_view /
// handle_edit_key) are defined in inspector_overlay_field_edit.cpp.

// The 20× zoom loupe (set_zoom_active / set_zoom_factor /
// resolve_view_color_at / update_zoom_sample / paint_zoom_panel) is
// defined in inspector_overlay_zoom.cpp.

} // namespace pulp::inspect
