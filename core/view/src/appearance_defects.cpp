#include <pulp/view/appearance_defects.hpp>

#include <pulp/canvas/text_shaper.hpp>
#include <pulp/view/buttons.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/widgets.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>

namespace pulp::view {
namespace {

Rect intersect_rect(Rect a, Rect b) {
    const float x1 = std::max(a.x, b.x);
    const float y1 = std::max(a.y, b.y);
    const float x2 = std::min(a.x + a.width, b.x + b.width);
    const float y2 = std::min(a.y + a.height, b.y + b.height);
    return {x1, y1, std::max(0.0f, x2 - x1), std::max(0.0f, y2 - y1)};
}

std::string rect_str(Rect r) {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    os << "{" << r.x << "," << r.y << " " << r.width << "x" << r.height << "}";
    return os.str();
}

std::string run_kind(const View& view) {
    if (dynamic_cast<const TextEditor*>(&view)) return "TextEditor";
    if (dynamic_cast<const TextButton*>(&view)) return "TextButton";
    if (dynamic_cast<const HyperlinkButton*>(&view)) return "HyperlinkButton";
    return ViewInspector::type_name(view);
}

std::string node_label(const View& view) {
    if (!view.id().empty()) return view.id();
    return std::string("<") + run_kind(view) + ">";
}

/// Font state a canvas command stream can be in when it draws text.
struct CanvasTextState {
    std::string family = "Inter";
    float size = 14.0f;
    int weight = 400;
    int slant = 0;
    float letter_spacing = 0.0f;
    int align = 0;      ///< 0 left, 1 center, 2 right
    int baseline = 0;   ///< 0 top, 1 middle, 2 bottom
    // Affine transform, canvas-local. Rotation and skew are tracked so a
    // rotated run can be reported as unmeasurable rather than mismeasured.
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, e = 0.0f, f = 0.0f;
    Rect clip{};
    bool clip_valid = false;
};

bool transform_is_axis_aligned(const CanvasTextState& s) {
    return std::abs(s.b) < 1e-4f && std::abs(s.c) < 1e-4f &&
           s.a > 0.0f && s.d > 0.0f;
}

/// A container that clips its children, and what the walk found beneath it.
/// Tracked so an absence can be attributed to a region rather than reported as
/// a single number for the whole screen.
struct ClipScope {
    const View* view = nullptr;
    Rect box{};             ///< the container's own box, absolute
    Rect clip{};            ///< its effective clip after every ancestor
    int parent = -1;        ///< index of the nearest enclosing scope, or -1
    int measured = 0;       ///< text runs beneath it that reached the screen
    int clipped = 0;        ///< text runs beneath it cut away entirely
    std::string sample_text;///< the first clipped string, so a finding is concrete
};

struct Walker {
    const AppearanceOptions& options;
    AppearanceReport& report;
    std::vector<ClipScope> scopes;
    std::vector<int> open_scopes;

    /// A run reached the screen. Every enclosing container gets the credit,
    /// because a container that shows even one string is not empty.
    void note_measured() {
        ++report.coverage.text_runs_measured;
        for (const int i : open_scopes)
            ++scopes[static_cast<std::size_t>(i)].measured;
    }

    /// A run was cut away entirely. Charged to every enclosing container; which
    /// of them is actually at fault is decided after the walk, by whether any
    /// sibling text survived.
    void note_clipped(const std::string& text) {
        ++report.coverage.skipped_clipped;
        for (const int i : open_scopes) {
            auto& scope = scopes[static_cast<std::size_t>(i)];
            ++scope.clipped;
            if (scope.sample_text.empty()) scope.sample_text = text;
        }
    }

    void record_skip(TextRunSkip why) {
        switch (why) {
            case TextRunSkip::invisible: ++report.coverage.skipped_invisible; break;
            case TextRunSkip::empty_text: ++report.coverage.skipped_empty; break;
            case TextRunSkip::clipped_away: ++report.coverage.skipped_clipped; break;
            case TextRunSkip::unmeasurable: ++report.coverage.skipped_unmeasurable; break;
            case TextRunSkip::degenerate_box: ++report.coverage.skipped_degenerate_box; break;
            case TextRunSkip::none: break;
        }
    }

    void emit(TextRun run, Rect clip, bool clip_active) {
        if (clip_active) {
            const Rect visible = intersect_rect(run.ink, clip);
            if (visible.width <= 0.0f || visible.height <= 0.0f) {
                note_clipped(run.text);
                return;
            }
            run.clip = clip;
            run.visible_ink = visible;
        } else {
            run.clip = run.ink;
            run.visible_ink = run.ink;
        }
        note_measured();
        report.runs.push_back(std::move(run));
    }

    /// Shape a plain string with an explicit face. Used for the widgets that do
    /// expose their typography; the ones that do not are counted as skipped.
    static float shaped_width(const std::string& text, const std::string& family,
                              float size, int weight, int slant, float spacing) {
        auto& shaper = canvas::global_text_shaper();
        auto prepared = shaper.prepare(text, family, size, weight, slant, spacing, {});
        return prepared.total_width();
    }

    void collect_canvas_text(const CanvasWidget& widget, Rect abs, Rect clip,
                             bool clip_active, int paint_order,
                             const View* scroll_frame) {
        ++report.coverage.canvas_widgets;
        auto& shaper = canvas::global_text_shaper();

        CanvasTextState state;
        std::vector<CanvasTextState> stack;
        int text_index = 0;

        for (const auto& cmd : widget.commands()) {
            switch (cmd.type) {
                case CanvasDrawCmd::Type::save:
                    stack.push_back(state);
                    break;
                case CanvasDrawCmd::Type::restore:
                    if (!stack.empty()) { state = stack.back(); stack.pop_back(); }
                    break;
                case CanvasDrawCmd::Type::translate:
                    state.e += state.a * cmd.x + state.c * cmd.y;
                    state.f += state.b * cmd.x + state.d * cmd.y;
                    break;
                case CanvasDrawCmd::Type::scale:
                    state.a *= cmd.x; state.b *= cmd.x;
                    state.c *= cmd.y; state.d *= cmd.y;
                    break;
                case CanvasDrawCmd::Type::rotate: {
                    const float cs = std::cos(cmd.extra), sn = std::sin(cmd.extra);
                    const float na = state.a * cs + state.c * sn;
                    const float nb = state.b * cs + state.d * sn;
                    const float nc = state.a * -sn + state.c * cs;
                    const float nd = state.b * -sn + state.d * cs;
                    state.a = na; state.b = nb; state.c = nc; state.d = nd;
                    break;
                }
                case CanvasDrawCmd::Type::set_transform:
                    state.a = cmd.x; state.b = cmd.y; state.c = cmd.w;
                    state.d = cmd.h; state.e = cmd.x2; state.f = cmd.y2;
                    break;
                case CanvasDrawCmd::Type::clip_rect:
                case CanvasDrawCmd::Type::clip_path: {
                    if (!transform_is_axis_aligned(state)) break;
                    const Rect r{state.a * cmd.x + state.e,
                                 state.d * cmd.y + state.f,
                                 state.a * cmd.w, state.d * cmd.h};
                    state.clip = state.clip_valid ? intersect_rect(state.clip, r) : r;
                    state.clip_valid = true;
                    break;
                }
                case CanvasDrawCmd::Type::set_font:
                    state.family = cmd.text.empty() ? state.family : cmd.text;
                    state.size = cmd.extra > 0.0f ? cmd.extra : state.size;
                    state.weight = 400;
                    state.slant = 0;
                    state.letter_spacing = 0.0f;
                    break;
                case CanvasDrawCmd::Type::set_font_full:
                    state.family = cmd.text.empty() ? state.family : cmd.text;
                    state.size = cmd.extra > 0.0f ? cmd.extra : state.size;
                    state.weight = static_cast<int>(cmd.x);
                    state.slant = static_cast<int>(cmd.y);
                    state.letter_spacing = cmd.x2;
                    break;
                case CanvasDrawCmd::Type::set_text_align:
                    state.align = cmd.int_val;
                    break;
                case CanvasDrawCmd::Type::set_text_baseline:
                    state.baseline = cmd.int_val;
                    break;
                case CanvasDrawCmd::Type::fill_text:
                case CanvasDrawCmd::Type::stroke_text: {
                    ++report.coverage.canvas_text_commands;
                    const int index = text_index++;
                    if (cmd.text.empty()) {
                        ++report.coverage.canvas_text_skipped;
                        ++report.coverage.skipped_empty;
                        break;
                    }
                    if (!transform_is_axis_aligned(state)) {
                        // A rotated or mirrored run's axis-aligned box is not
                        // its ink. Report it as unseen rather than wrong.
                        ++report.coverage.canvas_text_skipped;
                        ++report.coverage.skipped_unmeasurable;
                        break;
                    }
                    const float size = cmd.extra > 0.0f ? cmd.extra : state.size;
                    auto prepared = shaper.prepare(cmd.text, state.family, size,
                                                   state.weight, state.slant,
                                                   state.letter_spacing, {});
                    float w = prepared.total_width();
                    if (w <= 0.0f) {
                        ++report.coverage.canvas_text_skipped;
                        ++report.coverage.skipped_unmeasurable;
                        break;
                    }
                    if (cmd.w > 0.0f) w = std::min(w, cmd.w);  // Canvas2D maxWidth
                    // Vertical extents come from the same shaped line the
                    // painter draws, so the ink box tracks the real face
                    // metrics rather than a font-size guess.
                    auto line_layout = shaper.layout(prepared, 0.0f, 0.0f, 1);
                    float ascent = size * 0.85f;
                    float descent = size * 0.2f;
                    if (!line_layout.lines.empty()) {
                        const auto& line = line_layout.lines.front();
                        if (line.ascent > 0.0f) ascent = line.ascent;
                        if (line.descent > 0.0f) descent = line.descent;
                    }
                    const float h = ascent + descent;

                    // Canvas-local ink origin: textAlign moves x, textBaseline
                    // moves y. Both are replayed by the paint loop, so a
                    // detector that ignores them measures a box the glyphs are
                    // not in.
                    float local_x = cmd.x;
                    if (state.align == 1) local_x -= w * 0.5f;
                    else if (state.align == 2) local_x -= w;
                    float local_top = cmd.y;
                    if (state.baseline == 1) local_top -= (ascent - descent) * 0.5f;
                    else if (state.baseline == 2) local_top -= ascent + descent;

                    const Rect local{state.a * local_x + state.e,
                                     state.d * local_top + state.f,
                                     state.a * w, state.d * h};
                    Rect ink{abs.x + local.x, abs.y + local.y,
                             local.width, local.height};

                    Rect effective = clip_active ? clip : ink;
                    if (state.clip_valid) {
                        const Rect canvas_clip{abs.x + state.clip.x,
                                               abs.y + state.clip.y,
                                               state.clip.width, state.clip.height};
                        effective = clip_active ? intersect_rect(effective, canvas_clip)
                                                : canvas_clip;
                    }
                    const bool any_clip = clip_active || state.clip_valid;
                    Rect visible = ink;
                    if (any_clip) {
                        visible = intersect_rect(ink, effective);
                        if (visible.width <= 0.0f || visible.height <= 0.0f) {
                            ++report.coverage.canvas_text_skipped;
                            note_clipped(cmd.text);
                            break;
                        }
                    }

                    TextRun run;
                    run.node_id = node_label(widget) + "#text" + std::to_string(index);
                    run.kind = cmd.type == CanvasDrawCmd::Type::fill_text
                        ? "canvas:fill_text" : "canvas:stroke_text";
                    run.text = cmd.text;
                    run.ink = ink;
                    // A canvas run has no layout box of its own; the widget's
                    // own bounds is the surface it must stay inside.
                    run.box = abs;
                    run.clip = any_clip ? effective : ink;
                    run.visible_ink = visible;
                    run.paint_order = paint_order;
                    run.scroll_frame = scroll_frame;
                    run.from_canvas = true;
                    ++report.coverage.canvas_text_measured;
                    note_measured();
                    report.runs.push_back(std::move(run));
                    break;
                }
                default:
                    break;
            }
        }
    }

    void walk(const View& view, Rect parent_abs, Rect inherited_clip,
              bool clip_active, const View* scroll_frame, int& paint_order) {
        const auto b = view.bounds();
        const Rect abs{parent_abs.x + b.x, parent_abs.y + b.y, b.width, b.height};
        const int order = paint_order++;

        Rect clip = inherited_clip;
        bool active = clip_active;
        const bool clips_children =
            view.overflow() == View::Overflow::hidden ||
            view.overflow() == View::Overflow::scroll;
        if (clips_children) {
            clip = active ? intersect_rect(inherited_clip, abs) : abs;
            active = true;
        }

        if (!view.visible()) {
            // Count the whole subtree as unseen rather than walking it: nothing
            // under a hidden node is painted, and pretending otherwise
            // manufactures collisions that no user can see.
            count_hidden_subtree(view);
            return;
        }

        // Opened only for a visible container: a hidden one paints nothing by
        // definition, which the invisible skip already records.
        int scope_index = -1;
        if (clips_children) {
            ClipScope scope;
            scope.view = &view;
            scope.box = abs;
            scope.clip = clip;
            scope.parent = open_scopes.empty() ? -1 : open_scopes.back();
            scopes.push_back(std::move(scope));
            scope_index = static_cast<int>(scopes.size()) - 1;
            open_scopes.push_back(scope_index);
        }

        collect_from(view, abs, clip, active, order, scroll_frame);

        // A ScrollView translates its children by (-scroll_x, -scroll_y) at
        // PAINT time, so a child's bounds is in unscrolled content space. Apply
        // the offset here and every run below is in real screen space — which
        // is the only space in which two runs can be compared at all.
        Rect child_origin = abs;
        const View* child_frame = scroll_frame;
        if (auto* scroller = dynamic_cast<const ScrollView*>(&view)) {
            child_origin.x -= scroller->scroll_x();
            child_origin.y -= scroller->scroll_y();
            child_frame = &view;
        }

        for (auto* child : view.sorted_children_by_z_index())
            walk(*child, child_origin, clip, active, child_frame, paint_order);

        if (scope_index >= 0) open_scopes.pop_back();
    }

    void count_hidden_subtree(const View& view) {
        if (view_has_text(view)) {
            ++report.coverage.text_nodes_seen;
            ++report.coverage.skipped_invisible;
        }
        for (std::size_t i = 0; i < view.child_count(); ++i)
            count_hidden_subtree(*view.child_at(i));
    }

    static bool view_has_text(const View& view) {
        if (dynamic_cast<const Label*>(&view)) return true;
        if (dynamic_cast<const TextEditor*>(&view)) return true;
        if (dynamic_cast<const TextButton*>(&view)) return true;
        if (dynamic_cast<const HyperlinkButton*>(&view)) return true;
        return false;
    }

    void collect_from(const View& view, Rect abs, Rect clip, bool clip_active,
                      int paint_order, const View* scroll_frame) {
        if (options.include_canvas_text) {
            if (auto* cw = dynamic_cast<const CanvasWidget*>(&view))
                collect_canvas_text(*cw, abs, clip, clip_active, paint_order,
                                    scroll_frame);
        }

        if (auto* label = dynamic_cast<const Label*>(&view)) {
            ++report.coverage.text_nodes_seen;
            if (label->text().empty()) { record_skip(TextRunSkip::empty_text); return; }
            if (abs.width <= 0.0f || abs.height <= 0.0f) {
                record_skip(TextRunSkip::degenerate_box);
                return;
            }
            const auto extents = label->painted_text_extents(abs.width);
            if (!extents.measured || extents.width <= 0.0f) {
                record_skip(TextRunSkip::unmeasurable);
                return;
            }
            TextRun run;
            run.node_id = node_label(view);
            run.kind = run_kind(view);
            run.text = label->text();
            run.ink = Rect{abs.x + extents.ink.x, abs.y + extents.ink.y,
                           extents.width, extents.height};
            run.box = abs;
            run.paint_order = paint_order;
            run.scroll_frame = scroll_frame;
            emit(std::move(run), clip, clip_active);
            return;
        }

        if (auto* editor = dynamic_cast<const TextEditor*>(&view)) {
            ++report.coverage.text_nodes_seen;
            if (editor->text().empty()) { record_skip(TextRunSkip::empty_text); return; }
            if (abs.width <= 0.0f || abs.height <= 0.0f) {
                record_skip(TextRunSkip::degenerate_box);
                return;
            }
            std::string family = editor->font_family();
            if (family.empty()) {
                if (auto inh = view.inheritable_font_family(); inh.has_value())
                    family = *inh;
                else
                    family = "Inter";
            }
            const float w = shaped_width(editor->text(), family,
                                         editor->font_size(), 400, 0, 0.0f);
            if (w <= 0.0f) { record_skip(TextRunSkip::unmeasurable); return; }
            TextRun run;
            run.node_id = node_label(view);
            run.kind = "TextEditor";
            run.text = editor->text();
            run.ink = Rect{abs.x, abs.y, w, abs.height};
            run.box = abs;
            run.paint_order = paint_order;
            run.scroll_frame = scroll_frame;
            emit(std::move(run), clip, clip_active);
            return;
        }

        // TextButton and HyperlinkButton paint text but expose no typography,
        // so their ink cannot be measured from the public surface. Counting
        // them as measured using their own box width would make the width
        // detector tautologically green on every button in the tree.
        if (dynamic_cast<const TextButton*>(&view) ||
            dynamic_cast<const HyperlinkButton*>(&view)) {
            ++report.coverage.text_nodes_seen;
            record_skip(TextRunSkip::unmeasurable);
        }
    }
};

} // namespace

const char* to_string(AppearanceDefectKind kind) {
    switch (kind) {
        case AppearanceDefectKind::text_overlap: return "text_overlap";
        case AppearanceDefectKind::painted_wider_than_box: return "painted_wider_than_box";
        case AppearanceDefectKind::canvas_text_overlap: return "canvas_text_overlap";
        case AppearanceDefectKind::container_paints_no_text:
            return "container_paints_no_text";
    }
    return "unknown";
}

bool AppearanceCoverage::trustworthy() const {
    if (text_runs_measured <= 0) return false;
    const int total = text_runs_total();
    if (total <= 0) return false;
    // A container that paints none of its text is a region the walk could not
    // see into, and one is enough: an empty finding list says nothing about
    // what that container holds, however small it is against the rest of the
    // screen. So it vetoes rather than adding to a ratio, which is the only
    // form that protects a caller who reads this boolean and nothing else.
    //
    // Clipping on its own stays innocent. Counting every clipped run as
    // blindness would make a scrollable list untrustworthy for the ordinary
    // reason that most of it is below the fold, which is a worse answer than
    // the one being fixed.
    if (skipped_clipped_in_empty_container > 0) return false;
    // Skips that are not evidence of blindness: text that is genuinely absent,
    // hidden, or scrolled off screen was correctly not evaluated.
    const int blind = skipped_unmeasurable + skipped_degenerate_box;
    return blind * 2 < total;
}

std::string AppearanceCoverage::to_string() const {
    std::ostringstream os;
    os << "coverage: " << text_runs_measured << " of " << text_runs_total()
       << " text runs measured"
       << " (skipped: " << skipped_invisible << " invisible, "
       << skipped_empty << " empty, "
       << skipped_clipped << " clipped away ("
       << skipped_clipped_in_empty_container
       << " of them inside a container that paints nothing), "
       << skipped_unmeasurable << " unmeasurable, "
       << skipped_degenerate_box << " zero-area box)"
       << "; canvas: " << canvas_text_measured << " of " << canvas_text_commands
       << " text commands measured across " << canvas_widgets << " widget(s)"
       << "; pairs: " << pairs_compared << " compared, "
       << pairs_skipped_cross_frame << " skipped as cross-scroll-frame"
       << "; shaping: " << (shaping_is_real ? "real" : "ESTIMATED");
    if (skipped_clipped_in_empty_container > 0)
        os << "\n  BLIND REGION: " << skipped_clipped_in_empty_container
           << " text run(s) sit inside a container that paints none of its"
              " text. Nothing here is evidence about what that container"
              " holds.";
    if (!trustworthy())
        os << "\n  NOT TRUSTWORTHY: too little of the surface was measured for"
              " an empty result to mean anything.";
    return os.str();
}

std::string AppearanceFinding::describe() const {
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(1);
    os << to_string(kind) << ": ";
    if (kind == AppearanceDefectKind::container_paints_no_text) {
        os << a_id << " is on screen at " << rect_str(a_rect)
           << " but all " << clipped_runs
           << " of its text runs are clipped away (e.g. \"" << a_text
           << "\"); its clip is " << rect_str(b_rect);
    } else if (kind == AppearanceDefectKind::painted_wider_than_box) {
        os << a_id << " \"" << a_text << "\" paints " << painted_width
           << "px of ink outside its " << box_width << "px box by "
           << overflow_px << "px: ink " << rect_str(a_rect) << " vs box "
           << rect_str(b_rect);
    } else {
        os << a_id << " \"" << a_text << "\" " << rect_str(a_rect)
           << "  x  " << b_id << " \"" << b_text << "\" " << rect_str(b_rect)
           << "  overlap " << rect_str(overlap);
    }
    return os.str();
}

std::string AppearanceReport::to_string() const {
    std::ostringstream os;
    os << coverage.to_string() << "\n";
    if (findings.empty()) {
        os << "findings: none";
    } else {
        os << "findings: " << findings.size() << "\n";
        for (const auto& f : findings) os << "  " << f.describe() << "\n";
    }
    return os.str();
}

AppearanceReport detect_appearance_defects(const View& root,
                                           const AppearanceOptions& options) {
    AppearanceReport report;
    report.coverage.shaping_is_real = canvas::global_text_shaper().uses_real_shaping();

    Walker walker{options, report, {}, {}};
    int paint_order = 0;
    const Rect root_abs{0.0f, 0.0f, root.bounds().width, root.bounds().height};
    walker.walk(root, {0.0f, 0.0f, 0.0f, 0.0f}, root_abs, false, nullptr,
                paint_order);

    // Detector 4 — a container that is on screen but paints none of its text.
    //
    // This is the defect the collision detectors are structurally unable to
    // see. Text that lands nowhere produces no pair to compare and no box to
    // outgrow, so an empty panel and a correct one are the same empty finding
    // list. The signature separating it from an ordinary scroll frame is that
    // NOTHING beneath the container survived its clip: a list showing some of
    // its rows measured at least one run and is not reported here.
    {
        const auto& scopes = walker.scopes;
        std::vector<bool> qualifies(scopes.size(), false);
        for (std::size_t i = 0; i < scopes.size(); ++i) {
            const auto& scope = scopes[i];
            qualifies[i] = scope.clipped > 0 && scope.measured == 0 &&
                           scope.box.width > 0.0f && scope.box.height > 0.0f &&
                           scope.clip.width > 0.0f && scope.clip.height > 0.0f;
        }
        for (std::size_t i = 0; i < scopes.size(); ++i) {
            if (!qualifies[i]) continue;
            // Name the outermost container only. A collapsed panel takes every
            // frame nested inside it down too, and listing each one describes
            // one defect several times over.
            bool inside_another = false;
            for (int p = scopes[i].parent; p >= 0; p = scopes[static_cast<std::size_t>(p)].parent) {
                if (qualifies[static_cast<std::size_t>(p)]) { inside_another = true; break; }
            }
            if (inside_another) continue;

            const auto& scope = scopes[i];
            // Reported scopes never nest, so no clipped run is counted twice.
            report.coverage.skipped_clipped_in_empty_container += scope.clipped;

            AppearanceFinding f;
            f.kind = AppearanceDefectKind::container_paints_no_text;
            f.a_id = node_label(*scope.view);
            f.a_text = scope.sample_text;
            f.a_rect = scope.box;
            f.b_rect = scope.clip;
            f.clipped_runs = scope.clipped;
            report.findings.push_back(std::move(f));
        }
    }

    // Detector 2 — glyphs painted outside the box that is meant to hold them.
    //
    // Comparing widths alone answers a narrower question than the defect: a
    // run can be narrower than its box and still hang off an edge, which is
    // what an axis label pushed past the surface it is drawn on looks like.
    // The property is the ink's extents against the box's, so both the
    // outgrown-box case and the pushed-off-the-edge case are the same finding.
    for (const auto& run : report.runs) {
        if (run.box.width <= 0.0f) continue;
        const float tol = options.width_tolerance_px;
        const float over_left = run.box.x - run.ink.x;
        const float over_right =
            (run.ink.x + run.ink.width) - (run.box.x + run.box.width);
        const float overflow = std::max(over_left, over_right);
        if (overflow > tol) {
            AppearanceFinding f;
            f.kind = AppearanceDefectKind::painted_wider_than_box;
            f.a_id = run.node_id;
            f.a_text = run.text;
            f.a_rect = run.ink;
            f.b_rect = run.box;
            f.painted_width = run.ink.width;
            f.box_width = run.box.width;
            f.overflow_px = overflow;
            report.findings.push_back(std::move(f));
        }
    }

    // The positive control for an empty finding list, applied before the
    // collision pass so the pair loop is exercised on exactly the runs the
    // real measurement produced.
    if (options.control_inflate_ink_px > 0.0f) {
        const float g = options.control_inflate_ink_px;
        for (auto& run : report.runs) {
            run.visible_ink = Rect{run.visible_ink.x - g, run.visible_ink.y - g,
                                   run.visible_ink.width + 2.0f * g,
                                   run.visible_ink.height + 2.0f * g};
        }
    }

    // Detectors 1 and 3 — two runs sharing pixels.
    const auto& runs = report.runs;
    for (std::size_t i = 0; i < runs.size(); ++i) {
        for (std::size_t j = i + 1; j < runs.size(); ++j) {
            const auto& a = runs[i];
            const auto& b = runs[j];
            if (a.scroll_frame != b.scroll_frame &&
                !options.compare_across_scroll_frames) {
                ++report.coverage.pairs_skipped_cross_frame;
                continue;
            }
            ++report.coverage.pairs_compared;
            const Rect hit = intersect_rect(a.visible_ink, b.visible_ink);
            if (hit.width <= options.overlap_tolerance_px ||
                hit.height <= options.overlap_tolerance_px)
                continue;
            if (hit.width * hit.height < options.min_overlap_area_px2) continue;

            AppearanceFinding f;
            f.kind = (a.from_canvas && b.from_canvas)
                ? AppearanceDefectKind::canvas_text_overlap
                : AppearanceDefectKind::text_overlap;
            f.a_id = a.node_id; f.a_text = a.text; f.a_rect = a.ink;
            f.b_id = b.node_id; f.b_text = b.text; f.b_rect = b.ink;
            f.overlap = hit;
            report.findings.push_back(std::move(f));
        }
    }

    return report;
}

} // namespace pulp::view
