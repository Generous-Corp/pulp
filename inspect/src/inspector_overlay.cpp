// inspector_overlay.cpp — Visual inspector overlay implementation

#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/render/render_pass.hpp>

#include <algorithm>
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
        selected_ = nullptr;
        hovered_ = nullptr;
        alt_hover_target_ = nullptr;
        distance_anchor_ = nullptr;
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

    // Escape exits inspector mode
    if (active_ && event.key == KeyCode::escape && event.is_down) {
        set_active(false);
        return true;
    }

    return false;
}

bool InspectorOverlay::handle_mouse_event(const MouseEvent& event) {
    if (!active_) return false;

    auto pos = event.position;

    // Check if mouse is in the panel area
    if (point_in_panel(pos)) {
        if (event.is_down) {
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
    if (!selected_) {
        canvas.set_fill_color(kPanelDim);
        canvas.set_font("monospace", kFontSize);
        canvas.fill_text("Click a view to inspect", x, y + 16);
        return;
    }

    canvas.set_font("monospace", kFontSize);
    float line_y = y + 4;
    float line_h = 15.0f;

    auto draw_label = [&](const std::string& label, const std::string& value) {
        if (line_y > y + h) return;
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

    // Type and ID
    auto type = ViewInspector::type_name(*selected_);
    draw_heading(type + (selected_->id().empty() ? "" : " #" + selected_->id()));

    // Bounds
    auto r = selected_->bounds();
    auto abs = view_bounds_in_root(selected_);
    draw_label("bounds", std::to_string(static_cast<int>(r.x)) + ", " +
               std::to_string(static_cast<int>(r.y)) + ", " +
               std::to_string(static_cast<int>(r.width)) + " × " +
               std::to_string(static_cast<int>(r.height)));
    draw_label("absolute", std::to_string(static_cast<int>(abs.x)) + ", " +
               std::to_string(static_cast<int>(abs.y)));

    // Visibility
    draw_label("visible", selected_->visible() ? "true" : "false");
    if (selected_->opacity() < 1.0f)
        draw_label("opacity", std::to_string(selected_->opacity()));

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
    draw_label("direction", dir_str(f.direction));

    if (f.flex_grow > 0) draw_label("grow", std::to_string(f.flex_grow));
    if (f.flex_shrink != 1.0f) draw_label("shrink", std::to_string(f.flex_shrink));
    if (f.gap > 0) draw_label("gap", std::to_string(static_cast<int>(f.gap)));

    float pt2 = f.padding_top >= 0 ? f.padding_top : f.padding;
    float pr2 = f.padding_right >= 0 ? f.padding_right : f.padding;
    float pb2 = f.padding_bottom >= 0 ? f.padding_bottom : f.padding;
    float pl2 = f.padding_left >= 0 ? f.padding_left : f.padding;
    if (pt2 > 0 || pr2 > 0 || pb2 > 0 || pl2 > 0) {
        draw_label("padding", std::to_string(static_cast<int>(pt2)) + " " +
                   std::to_string(static_cast<int>(pr2)) + " " +
                   std::to_string(static_cast<int>(pb2)) + " " +
                   std::to_string(static_cast<int>(pl2)));
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

} // namespace pulp::inspect
