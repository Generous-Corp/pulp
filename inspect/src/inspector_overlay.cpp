// inspector_overlay.cpp — Visual inspector overlay implementation

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/render/render_pass.hpp>

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace pulp::inspect {

// ── Colors ──────────────────────────────────────────────────────────────────

static const Color kHighlightFill    = Color::rgba(0.25f, 0.5f, 1.0f, 0.12f);
static const Color kHighlightStroke  = Color::rgba(0.25f, 0.5f, 1.0f, 0.7f);
static const Color kSelectedFill     = Color::rgba(1.0f, 0.5f, 0.0f, 0.12f);
static const Color kSelectedStroke   = Color::rgba(1.0f, 0.5f, 0.0f, 0.7f);
static const Color kPanelBg          = Color::rgba(0.08f, 0.08f, 0.1f, 0.94f);
static const Color kPanelText        = Color::rgba(0.85f, 0.85f, 0.9f, 1.0f);
static const Color kPanelDim         = Color::rgba(0.5f, 0.5f, 0.55f, 1.0f);
static const Color kPanelHighlight   = Color::rgba(0.25f, 0.5f, 1.0f, 0.25f);
static const Color kTreeSelected     = Color::rgba(1.0f, 0.5f, 0.0f, 0.3f);
static const Color kDistanceLine     = Color::rgba(1.0f, 0.2f, 0.3f, 0.8f);
static const Color kPaddingColor     = Color::rgba(0.2f, 0.8f, 0.3f, 0.15f);
static const Color kMarginColor      = Color::rgba(1.0f, 0.6f, 0.1f, 0.15f);
static const Color kStatsBg          = Color::rgba(0.0f, 0.0f, 0.0f, 0.7f);
static const Color kStatsText        = Color::rgba(0.6f, 1.0f, 0.6f, 1.0f);
static const Color kStatsWarn        = Color::rgba(1.0f, 0.4f, 0.3f, 1.0f);

// Phase 3b — editable-field visual treatment
static const Color kFieldEditCaret   = Color::rgba(0.95f, 0.6f, 0.2f, 1.0f);
static const Color kFieldEditUnder   = Color::rgba(0.95f, 0.6f, 0.2f, 0.9f);
static const Color kFieldEditBg      = Color::rgba(0.95f, 0.6f, 0.2f, 0.18f);

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
    }
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

    return false;
}

bool InspectorOverlay::handle_mouse_event(const MouseEvent& event) {
    if (!active_) return false;

    auto pos = event.position;

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

void InspectorOverlay::paint(Canvas& canvas) {
    if (!active_) return;

    rebuild_flat_tree();
    paint_highlight(canvas);
    paint_distance_lines(canvas);
    if (selected_) paint_box_model(canvas, selected_);
    paint_panel(canvas);
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

    // Tree section (top half)
    float tree_height = root_h * 0.5f;
    float cursor_y = 4.0f;
    paint_tree_section(canvas, panel_x + 8, 4, panel_width_ - 16, cursor_y);

    // Divider
    float props_y = tree_height;
    canvas.set_stroke_color(Color::rgba(0.3f, 0.3f, 0.35f, 0.5f));
    canvas.stroke_line(panel_x + 8, props_y, root_w - 8, props_y);

    // Props section (middle)
    float stats_y = root_h - kStatsBarHeight;
    paint_props_section(canvas, panel_x + 8, props_y + 4, panel_width_ - 16, stats_y - props_y - 8);

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

    // View count
    auto count = ViewInspector::count_views(root_);
    canvas.set_fill_color(kPanelDim);
    canvas.fill_text(std::to_string(count) + " views", x + w - 60, y + 16);
}

// ── Phase 3b — Live-editable box-model fields ───────────────────────────────
//
// Click on a numeric value in the property panel → enter edit mode.
// Typed digits / decimal / sign extend the buffer; arrows nudge (±1
// plain, ±10 Shift, ±100 Cmd matching Figma); Enter commits (tweak
// + persisted); Esc cancels (revert); Tab commits + moves to the
// next editable field. The tweak is emitted via the SAME
// emit_tweak_for_selection() path Phase 3a drag-handles use, so
// downstream persistence (Phase 1 disk write) gets both gestures for
// free.
//
// Real-time preview: we write to the underlying View (flex().padding
// = N etc.) on every keystroke + arrow nudge so Yoga reflows live —
// matching the spec's "updates in real-time as Yoga reflow runs"
// requirement. The tweak (anchor-keyed persisted edit) is emitted
// only on commit, so a user who Esc-cancels never poisons the store.

int InspectorOverlay::editable_field_at(Point p) const {
    for (std::size_t i = 0; i < editable_fields_.size(); ++i) {
        const auto& f = editable_fields_[i];
        if (p.x >= f.bounds.x && p.x <= f.bounds.x + f.bounds.width &&
            p.y >= f.bounds.y && p.y <= f.bounds.y + f.bounds.height)
            return static_cast<int>(i);
    }
    return -1;
}

float InspectorOverlay::read_field_value(std::string_view field_path) const {
    if (!selected_) return 0.0f;
    const auto& f = selected_->flex();
    if (field_path == "layout.width")    return f.preferred_width;
    if (field_path == "layout.height")   return f.preferred_height;
    if (field_path == "layout.padding")  return f.padding;
    if (field_path == "layout.margin")   return f.margin;
    if (field_path == "style.opacity")   return selected_->opacity();
    // style.font_size is documented in the spec but View has no
    // direct font_size accessor at the View level — Label / TextEditor
    // own their own font sizes. Skip until widget-aware property
    // mapping ships (noted in PR body).
    return 0.0f;
}

void InspectorOverlay::write_field_value(std::string_view field_path, float value) {
    if (!selected_) return;
    auto& f = selected_->flex();
    bool layout_touched = false;
    if (field_path == "layout.width") {
        f.preferred_width = value; layout_touched = true;
    } else if (field_path == "layout.height") {
        f.preferred_height = value; layout_touched = true;
    } else if (field_path == "layout.padding") {
        f.padding = value; layout_touched = true;
    } else if (field_path == "layout.margin") {
        f.margin = value; layout_touched = true;
    } else if (field_path == "style.opacity") {
        // Opacity clamps to [0,1] inside View::set_opacity.
        selected_->set_opacity(value);
    }
    if (layout_touched) {
        selected_->invalidate_layout();
        // Yoga propagates from the dirty node up to the next absolute-
        // position container or the root; mark up to the root so the
        // next paint pass definitely recomputes. (Cheap — the flag is
        // just a bool.)
        for (View* v = selected_; v; v = v->parent())
            v->invalidate_layout();
    }
}

bool InspectorOverlay::begin_field_edit(std::string field_path, float initial_value) {
    if (!selected_) return false;
    if (field_path.empty()) return false;
    editing_field_ = std::move(field_path);
    edit_target_view_ = selected_;
    edit_original_value_ = initial_value;
    // Format as integer when the value is whole-number-ish; this
    // matches the static display logic in paint_props_section so the
    // user sees the same digits they were looking at a frame ago.
    std::ostringstream oss;
    if (std::abs(initial_value - std::round(initial_value)) < 1e-4f) {
        oss << static_cast<int>(std::round(initial_value));
    } else {
        oss << std::fixed << std::setprecision(2) << initial_value;
    }
    edit_buffer_ = oss.str();
    edit_caret_pos_ = edit_buffer_.size();
    return true;
}

bool InspectorOverlay::commit_field_edit() {
    if (editing_field_.empty()) return false;

    // Parse buffer; tolerate trailing whitespace / empty (treat empty
    // as "no change" — revert without emitting a tweak).
    bool emitted = false;
    if (!edit_buffer_.empty()) {
        char* end = nullptr;
        double parsed = std::strtod(edit_buffer_.c_str(), &end);
        if (end != edit_buffer_.c_str()) {
            float value = static_cast<float>(parsed);
            // Real-time preview already applied on each keystroke, but
            // ensure the final committed value is correct (in case the
            // last keystroke wasn't a digit).
            write_field_value(editing_field_, value);
            // Emit the tweak — persisted edit, anchor-keyed.
            emit_tweak_for_selection(editing_field_,
                                     choc::value::createFloat32(value),
                                     "inspector-keyboard-edit");
            emitted = true;
        }
    }

    editing_field_.clear();
    edit_buffer_.clear();
    edit_caret_pos_ = 0;
    edit_target_view_ = nullptr;
    return emitted;
}

void InspectorOverlay::cancel_field_edit() {
    if (editing_field_.empty()) return;
    // Revert the underlying View to the original value — real-time
    // preview may have mutated it.
    write_field_value(editing_field_, edit_original_value_);
    editing_field_.clear();
    edit_buffer_.clear();
    edit_caret_pos_ = 0;
    edit_target_view_ = nullptr;
}

void InspectorOverlay::apply_edit_buffer_to_view() {
    if (editing_field_.empty()) return;
    if (edit_buffer_.empty()) return;
    char* end = nullptr;
    double parsed = std::strtod(edit_buffer_.c_str(), &end);
    if (end == edit_buffer_.c_str()) return;  // not a number yet
    write_field_value(editing_field_, static_cast<float>(parsed));
}

// Translate a KeyCode into the digit / sign / decimal character it
// would produce in a US-layout context. Shift is intentionally
// ignored — the spec only needs unmodified digits + period + minus.
// (Full keyboard mapping is platform-layer concern; this is a
// pragmatic subset for box-model numeric entry.)
static char key_to_char(KeyCode k, bool shift) {
    int v = static_cast<int>(k);
    if (v >= '0' && v <= '9') return static_cast<char>(v);
    // We treat the keys as both their unshifted and shifted symbols
    // for "." and "-" because Pulp's KeyCode enum doesn't define
    // separate "period" / "minus" entries — platform code dispatches
    // them via text-input. Phase 3b accepts numeric editing only;
    // digits cover 99% of use. Decimal entry on platforms without a
    // text-input path can be added by extending KeyCode (filed as a
    // follow-up in the PR body if it becomes a real ask).
    (void)shift;
    return 0;
}

bool InspectorOverlay::handle_edit_key(const KeyEvent& event) {
    // ── Cancel: Esc ────────────────────────────────────────────────
    if (event.key == KeyCode::escape) {
        cancel_field_edit();
        return true;
    }

    // ── Commit: Enter ──────────────────────────────────────────────
    if (event.key == KeyCode::enter) {
        commit_field_edit();
        return true;
    }

    // ── Tab: commit + move to next field ──────────────────────────
    if (event.key == KeyCode::tab) {
        // Find current field index, then move +1 (Shift+Tab = -1)
        // among the editable_fields_ list (populated by the last
        // paint). The list is repopulated each paint so it's a stable
        // ordering matching what the user sees.
        std::string current = editing_field_;
        commit_field_edit();

        int idx = -1;
        for (std::size_t i = 0; i < editable_fields_.size(); ++i) {
            if (editable_fields_[i].path == current) { idx = static_cast<int>(i); break; }
        }
        if (idx >= 0 && !editable_fields_.empty()) {
            int step = event.isShiftDown() ? -1 : 1;
            int next = (idx + step) % static_cast<int>(editable_fields_.size());
            if (next < 0) next += static_cast<int>(editable_fields_.size());
            const auto& nf = editable_fields_[next];
            // Re-read the value: previous commit may have changed it.
            float v = read_field_value(nf.path);
            begin_field_edit(nf.path, v);
        }
        return true;
    }

    // ── Backspace: trim one char ──────────────────────────────────
    if (event.key == KeyCode::backspace) {
        if (edit_caret_pos_ > 0 && !edit_buffer_.empty()) {
            edit_buffer_.erase(edit_caret_pos_ - 1, 1);
            --edit_caret_pos_;
            apply_edit_buffer_to_view();
        }
        return true;
    }

    // ── Arrow nudging: ±1 / ±10 (shift) / ±100 (cmd) ──────────────
    if (event.key == KeyCode::up || event.key == KeyCode::down) {
        float step = 1.0f;
        if (event.isShiftDown()) step = 10.0f;
        if (event.isMainModifier()) step = 100.0f;
        if (event.key == KeyCode::down) step = -step;

        // Parse current buffer + apply step. Fall back to original
        // value if parse fails (e.g. buffer is empty / only "-").
        float current = edit_original_value_;
        if (!edit_buffer_.empty()) {
            char* end = nullptr;
            double parsed = std::strtod(edit_buffer_.c_str(), &end);
            if (end != edit_buffer_.c_str())
                current = static_cast<float>(parsed);
        }
        float new_v = current + step;

        // Re-format the buffer; keep integer formatting if both ends
        // are whole numbers so we don't surprise the user with
        // suddenly-decimal values from arrow nudging.
        std::ostringstream oss;
        if (std::abs(new_v - std::round(new_v)) < 1e-4f &&
            std::abs(current - std::round(current)) < 1e-4f) {
            oss << static_cast<int>(std::round(new_v));
        } else {
            oss << std::fixed << std::setprecision(2) << new_v;
        }
        edit_buffer_ = oss.str();
        edit_caret_pos_ = edit_buffer_.size();
        apply_edit_buffer_to_view();
        return true;
    }

    // ── Left / Right: caret movement ──────────────────────────────
    if (event.key == KeyCode::left) {
        if (edit_caret_pos_ > 0) --edit_caret_pos_;
        return true;
    }
    if (event.key == KeyCode::right) {
        if (edit_caret_pos_ < edit_buffer_.size()) ++edit_caret_pos_;
        return true;
    }

    // ── Home / End ────────────────────────────────────────────────
    if (event.key == KeyCode::home) {
        edit_caret_pos_ = 0;
        return true;
    }
    if (event.key == KeyCode::end_) {
        edit_caret_pos_ = edit_buffer_.size();
        return true;
    }

    // ── Digit insertion ───────────────────────────────────────────
    // Refuse digits when a modifier (Cmd / Ctrl) is held so we don't
    // swallow chord shortcuts.
    if (event.isCmdDown() || event.isCtrlDown()) return false;

    char c = key_to_char(event.key, event.isShiftDown());
    if (c >= '0' && c <= '9') {
        edit_buffer_.insert(edit_caret_pos_, 1, c);
        ++edit_caret_pos_;
        apply_edit_buffer_to_view();
        return true;
    }

    return false;
}

} // namespace pulp::inspect
