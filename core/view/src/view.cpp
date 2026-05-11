#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>
#include <pulp/view/plugin_view_host.hpp>
#include <algorithm>
#include <numeric>
#include <sstream>

namespace pulp::view {

namespace {

// Build a per-corner rounded-rect path on the canvas (issue-1026). When any
// of setBorderTopLeftRadius / TopRight / BottomLeft / BottomRight has been
// called on the View, the four corners can have independent radii — which
// the canvas's single-radius fill_rounded_rect / stroke_rounded_rect APIs
// cannot express on their own. We approximate each corner with a single
// cubic_to whose control magnitude is r * 0.55228 — the standard
// "kappa" approximation of a quarter-circle by a Bezier. Subclasses of
// Canvas that lack a real cubic_to fall back to line_to via the base class.
//
// Layout (TL, TR, BL, BR are clamped to half the box):
//
//      tl                 tr
//        +---------------+
//        |               |
//      bl|               |br
//        +---------------+
void build_per_corner_rounded_rect_path(
    pulp::canvas::Canvas& canvas,
    float w, float h,
    float tl, float tr, float bl, float br) {
    const float half_w = w * 0.5f;
    const float half_h = h * 0.5f;
    auto clamp = [&](float r) {
        const float lim = std::min(half_w, half_h);
        return std::max(0.0f, std::min(r, lim));
    };
    tl = clamp(tl);
    tr = clamp(tr);
    bl = clamp(bl);
    br = clamp(br);
    // Cubic kappa for quarter-circle approximation.
    constexpr float k = 0.5522847498f;

    canvas.begin_path();
    // Start at top edge after TL corner.
    canvas.move_to(tl, 0.0f);
    // Top edge → top-right corner.
    canvas.line_to(w - tr, 0.0f);
    if (tr > 0.0f)
        canvas.cubic_to(w - tr + tr * k, 0.0f,
                        w, tr - tr * k,
                        w, tr);
    // Right edge → bottom-right corner.
    canvas.line_to(w, h - br);
    if (br > 0.0f)
        canvas.cubic_to(w, h - br + br * k,
                        w - br + br * k, h,
                        w - br, h);
    // Bottom edge → bottom-left corner.
    canvas.line_to(bl, h);
    if (bl > 0.0f)
        canvas.cubic_to(bl - bl * k, h,
                        0.0f, h - bl + bl * k,
                        0.0f, h - bl);
    // Left edge → top-left corner.
    canvas.line_to(0.0f, tl);
    if (tl > 0.0f)
        canvas.cubic_to(0.0f, tl - tl * k,
                        tl - tl * k, 0.0f,
                        tl, 0.0f);
    canvas.close_path();
}

} // namespace

void View::paint_all(canvas::Canvas& canvas) {
    if (!visible_) return;

    canvas.save();
    canvas.translate(bounds_.x, bounds_.y);

    // Disabled state: reduce opacity (CSS :disabled equivalent)
    if (!enabled_) canvas.set_opacity(0.4f);

    // CSS transforms: translate, rotate, scale, skew — around transform-origin
    bool has_transform = (scale_ != 1.0f || rotation_deg_ != 0 ||
                          translate_x_ != 0 || translate_y_ != 0 ||
                          skew_x_ != 0 || skew_y_ != 0);
    if (has_transform) {
        float ox = bounds_.width * origin_x_;
        float oy = bounds_.height * origin_y_;
        canvas.translate(ox, oy);

        // Apply translate
        if (translate_x_ != 0 || translate_y_ != 0)
            canvas.translate(translate_x_, translate_y_);

        // Apply rotation
        if (rotation_deg_ != 0)
            canvas.rotate(rotation_deg_ * 3.14159265f / 180.0f);

        // Apply scale
        if (scale_ != 1.0f)
            canvas.scale(scale_, scale_);

        // Skew via scale hack (true skew needs matrix — approximate for now)
        // TODO: add canvas.skew() for proper CSS skew()

        canvas.translate(-ox, -oy);
    }

    // Full 2D affine transform matrix (issue-930). Composed onto the current
    // canvas matrix via concat_transform so parent transforms still apply
    // and children inherit. Used by setTransform(id,a,b,c,d,e,f) from JS,
    // primarily for translateX(-50%) centering and future animation.
    //
    // pulp #1026 — transform-origin now applies to the matrix path too,
    // BUT only when the caller has explicitly called setTransformOrigin.
    // Existing setTransform() call sites that never touched the origin
    // continue to get a plain concat (backward-compat with #930). When
    // an explicit origin is set, the canvas op equivalent of "transform
    // around origin (ox, oy)" is
    //   translate(ox, oy) ; concat(M) ; translate(-ox, -oy).
    if (has_transform_matrix_) {
        const bool apply_origin = origin_explicit_;
        const float ox = bounds_.width  * origin_x_;
        const float oy = bounds_.height * origin_y_;
        if (apply_origin) canvas.translate(ox, oy);
        canvas.concat_transform(transform_matrix_a_, transform_matrix_b_,
                                transform_matrix_c_, transform_matrix_d_,
                                transform_matrix_e_, transform_matrix_f_);
        if (apply_origin) canvas.translate(-ox, -oy);
    }

    // CSS `backdrop-filter: blur(N)` (issue-926). A separate compositing layer
    // whose initial content is the parent surface blurred — sits BELOW the
    // widget's own opacity/filter layer so background, border, and children
    // composite over the frosted backdrop. Paired with the matching restore()
    // at the end of paint_all.
    bool needs_backdrop_layer = (backdrop_blur_ > 0.0f);
    if (needs_backdrop_layer) {
        canvas.save_backdrop_filter(0, 0, bounds_.width, bounds_.height,
                                    backdrop_blur_);
    }

    // Compositing layer for opacity, blur, or post-effects.
    // pulp #936 P1 / #949 — both the outset box-shadow and the overflow
    // clip must be pushed AFTER this saveLayer so that the view's
    // own opacity / filter layer contains them. Pre-fix, the outset
    // shadow was painted onto the parent's compositing context (before
    // saveLayer) which broke CSS opacity stacking and could mask
    // subsequent child-layer content on certain Skia paths.
    // pulp #1549 — `mix-blend-mode` forces a saveLayer the same way
    // opacity / filter does so the subtree composites back through the
    // requested blend mode at restore() time. Default `BlendMode::normal`
    // is a paint-time no-op (kSrcOver) and stays out of `needs_layer`.
    const bool needs_blend_layer = has_non_default_blend_mode();
    bool needs_layer = (opacity_ < 1.0f) || (filter_blur_ > 0.0f)
                       || !filter_chain_.empty() || needs_layer_
                       || (effect_ && effect_->needs_layer())
                       || needs_blend_layer;
    if (needs_layer) {
        if (effect_) {
            effect_->configure_layer(canvas, 0, 0, bounds_.width, bounds_.height);
        } else if (!filter_chain_.empty()) {
            // pulp #1434 Phase A2-4 — full CSS filter chain. Translate
            // View::FilterOp into canvas::FilterChainEntry (parallel
            // shape) and hand off to the canvas backend; Skia composes
            // via SkImageFilters, CG falls back to blur-only.
            std::vector<pulp::canvas::Canvas::FilterChainEntry> chain;
            chain.reserve(filter_chain_.size());
            for (const auto& op : filter_chain_) {
                pulp::canvas::Canvas::FilterChainEntry e{};
                using ViewK = View::FilterOp::Kind;
                using CanvK = pulp::canvas::Canvas::FilterChainEntry::Kind;
                switch (op.kind) {
                    case ViewK::blur:        e.kind = CanvK::blur;        break;
                    case ViewK::brightness:  e.kind = CanvK::brightness;  break;
                    case ViewK::contrast:    e.kind = CanvK::contrast;    break;
                    case ViewK::grayscale:   e.kind = CanvK::grayscale;   break;
                    case ViewK::hue_rotate:  e.kind = CanvK::hue_rotate;  break;
                    case ViewK::invert:      e.kind = CanvK::invert;      break;
                    case ViewK::opacity:     e.kind = CanvK::opacity;     break;
                    case ViewK::saturate:    e.kind = CanvK::saturate;    break;
                    case ViewK::sepia:       e.kind = CanvK::sepia;       break;
                    case ViewK::drop_shadow: e.kind = CanvK::drop_shadow; break;
                }
                e.amount      = op.amount;
                e.angle_deg   = op.angle_deg;
                e.ds_offset_x = op.ds_offset_x;
                e.ds_offset_y = op.ds_offset_y;
                e.ds_blur     = op.ds_blur;
                e.ds_color    = op.ds_color;
                chain.push_back(e);
            }
            canvas.save_layer_with_filters(0, 0, bounds_.width, bounds_.height,
                                            opacity_, chain.data(),
                                            static_cast<int>(chain.size()));
        } else if (needs_blend_layer) {
            // pulp #1549 — saveLayer with explicit blend mode for
            // CSS / RN `mix-blend-mode`.
            canvas.save_layer_with_blend(0, 0, bounds_.width, bounds_.height,
                                         opacity_, filter_blur_, mix_blend_mode_);
        } else {
            canvas.save_layer(0, 0, bounds_.width, bounds_.height, opacity_, filter_blur_);
        }
    }

    // Outset drop shadows paint inside the compositing layer so the view's
    // opacity / filter / backdrop applies to them (CSS spec — shadows are
    // part of the element's stacking context). The shadow blur halo can
    // still extend past the box bounds when overflow is visible; when
    // overflow is hidden, the clip below limits the halo to the bounds
    // — same behavior browsers exhibit for clipped boxes. Inset shadows
    // paint later, on top of the content, see below.
    if (has_shadow_ && !shadow_.inset) {
        canvas.draw_box_shadow(0, 0, bounds_.width, bounds_.height,
                               shadow_.offset_x, shadow_.offset_y,
                               shadow_.blur, shadow_.spread,
                               shadow_.color, /*inset=*/false,
                               corner_radius_);
    }

    // Clip only when overflow:hidden / overflow:scroll is explicitly
    // opted into. Default is overflow:visible (CSS default, pulp #972)
    // so absolutely-positioned popover/dropdown children that extend
    // outside the parent's content bounds still paint. `scroll` clips
    // the painted box like `hidden` per CSS spec — we don't have a
    // scrollbar layer yet, but the layout-side overflow propagation
    // is wired through Yoga so descendants measure correctly.
    if (overflow_ == Overflow::hidden || overflow_ == Overflow::scroll)
        canvas.clip_rect(0, 0, bounds_.width, bounds_.height);

    // CSS `clip-path: path("...")` (pulp #1515). The View's local
    // coordinate space is (0,0)→(bounds_.width, bounds_.height) at
    // this point — the SVG-path-d string is interpreted in that
    // space, matching CSS where `clip-path` is anchored at the
    // border-box origin. The Skia backend parses via
    // SkPath::FromSVGString and intersects the canvas clip;
    // RecordingCanvas captures a `clip_path_svg` command for tests;
    // backends without a path parser silently no-op. The clip is
    // released by the matching `canvas.restore()` at the end of
    // paint_all (no separate save() needed — the outer
    // `canvas.save()` at function entry already covers it).
    if (!clip_path_.empty())
        canvas.clip_path_svg(clip_path_);

    // Per-corner border-radius (issue-1026): when any of the
    // setBorderTopLeftRadius / TopRight / BottomLeft / BottomRight setters
    // has been called we paint backgrounds and the border via a path
    // approximating each corner independently. Otherwise we keep using the
    // canvas's optimized fill_rounded_rect / stroke_rounded_rect with the
    // uniform corner_radius_.
    const bool use_per_corner = has_corner_radii_;

    // Paint background gradient if set (CSS background: linear-gradient)
    if (bg_gradient_type_ > 0 && !bg_gradient_colors_.empty()) {
        canvas.set_fill_gradient_linear(
            bg_grad_x0_ * bounds_.width, bg_grad_y0_ * bounds_.height,
            bg_grad_x1_ * bounds_.width, bg_grad_y1_ * bounds_.height,
            bg_gradient_colors_.data(), bg_gradient_positions_.data(),
            static_cast<int>(bg_gradient_colors_.size()));
        if (use_per_corner) {
            build_per_corner_rounded_rect_path(canvas, bounds_.width, bounds_.height,
                                               corner_radii_[0], corner_radii_[1],
                                               corner_radii_[2], corner_radii_[3]);
            canvas.fill_current_path();
        } else if (corner_radius_ > 0) {
            canvas.fill_rounded_rect(0, 0, bounds_.width, bounds_.height, corner_radius_);
        } else {
            canvas.fill_rect(0, 0, bounds_.width, bounds_.height);
        }
        canvas.clear_fill_gradient();
    }

    // Paint background if set
    if (has_bg_ && bg_gradient_type_ == 0) {
        canvas.set_fill_color(bg_color_);
        if (use_per_corner) {
            build_per_corner_rounded_rect_path(canvas, bounds_.width, bounds_.height,
                                               corner_radii_[0], corner_radii_[1],
                                               corner_radii_[2], corner_radii_[3]);
            canvas.fill_current_path();
        } else if (corner_radius_ > 0) {
            canvas.fill_rounded_rect(0, 0, bounds_.width, bounds_.height, corner_radius_);
        } else {
            canvas.fill_rect(0, 0, bounds_.width, bounds_.height);
        }
    }

    // Paint border if set
    // pulp #1434 Triage #10 — border-style honored at paint time.
    // `none` / `hidden` short-circuit; `dashed` / `dotted` install a
    // SkDashPathEffect via canvas.set_line_dash(...) before stroking.
    // Other named styles (`double` / `groove` / `ridge` / `inset` /
    // `outset`) currently degrade to solid — Skia / CG plumbing for
    // those is a follow-up paint slice.
    if (has_border_ && border_width_ > 0
            && border_style_ != BorderStyle::none
            && border_style_ != BorderStyle::hidden) {
        canvas.set_stroke_color(border_color_);
        canvas.set_line_width(border_width_);

        // Install dash pattern for dashed / dotted. Pattern values are
        // a function of the stroke width so the visible cadence scales
        // with the border thickness — matches how CSS UAs render these.
        const float w = border_width_;
        if (border_style_ == BorderStyle::dashed) {
            const float dashed[2] = { 3.0f * w, 3.0f * w };
            canvas.set_line_dash(dashed, 2, 0.0f);
        } else if (border_style_ == BorderStyle::dotted) {
            const float dotted[2] = { 1.0f * w, 2.0f * w };
            canvas.set_line_dash(dotted, 2, 0.0f);
        }

        if (use_per_corner) {
            build_per_corner_rounded_rect_path(canvas, bounds_.width, bounds_.height,
                                               corner_radii_[0], corner_radii_[1],
                                               corner_radii_[2], corner_radii_[3]);
            canvas.stroke_current_path();
        } else if (corner_radius_ > 0) {
            canvas.stroke_rounded_rect(0, 0, bounds_.width, bounds_.height, corner_radius_);
        } else {
            canvas.stroke_rect(0, 0, bounds_.width, bounds_.height);
        }

        // Reset dash pattern so subsequent strokes (per-side borders,
        // children) aren't dashed inadvertently. Empty intervals array
        // disables the path effect on Skia and is a no-op on CG.
        if (border_style_ == BorderStyle::dashed
                || border_style_ == BorderStyle::dotted) {
            canvas.set_line_dash(nullptr, 0, 0.0f);
        }
    }

    // Widget-specific painting
    paint(canvas);

    // Paint children — pulp #972. CSS z-index ordering: stable-sort
    // ascending by z_index() so siblings with equal z keep insertion
    // order (CSS painting-order rule). Higher z paints later, ending
    // up visually on top. The default z_index_ is 0, so views that
    // never call set_z_index() retain insertion order — no behaviour
    // change for legacy plugins. setZIndex() on the JS bridge has
    // existed as a no-op until now; this hooks it up.
    auto paint_order = sorted_children_by_z_index();
    for (View* child : paint_order) {
        child->paint_all(canvas);
    }

    // Inset box shadows paint over the content so the inner darkening
    // shows through children (CSS spec: inset shadows are above the
    // background but below the border-image, here approximated as above
    // children too).
    if (has_shadow_ && shadow_.inset) {
        canvas.draw_box_shadow(0, 0, bounds_.width, bounds_.height,
                               shadow_.offset_x, shadow_.offset_y,
                               shadow_.blur, shadow_.spread,
                               shadow_.color, /*inset=*/true,
                               corner_radius_);
    }

    // CSS / RN outline (pulp #1519). Paints OUTSIDE the border-box and
    // does NOT take up Yoga layout space (parent never reserves room
    // for it). The stroke is centered on the inflated rect, so the
    // visual outline edge lies at offset (outline_offset + outline_width)
    // beyond the border-box. Reuses border_style enum + dash plumbing —
    // CSS spec lists the same line-style keyword set for outline.
    // none/hidden/zero-width short-circuit. Paints after children so
    // it stays on top of everything inside the box.
    if (outline_width_ > 0
            && outline_style_ != BorderStyle::none
            && outline_style_ != BorderStyle::hidden) {
        canvas.set_stroke_color(outline_color_);
        canvas.set_line_width(outline_width_);

        const float w = outline_width_;
        if (outline_style_ == BorderStyle::dashed) {
            const float dashed[2] = { 3.0f * w, 3.0f * w };
            canvas.set_line_dash(dashed, 2, 0.0f);
        } else if (outline_style_ == BorderStyle::dotted) {
            const float dotted[2] = { 1.0f * w, 2.0f * w };
            canvas.set_line_dash(dotted, 2, 0.0f);
        }

        // Inflate around all four sides: stroke center at offset+w/2.
        const float inflate = outline_offset_ + outline_width_ * 0.5f;
        const float ox = -inflate;
        const float oy = -inflate;
        const float ow = bounds_.width + 2.0f * inflate;
        const float oh = bounds_.height + 2.0f * inflate;
        // Outline corner radius mirrors the border-box corner radius
        // expanded by the same inflate distance — matches CSS UA
        // behavior where the outline follows the box's corner curvature.
        if (corner_radius_ > 0) {
            canvas.stroke_rounded_rect(ox, oy, ow, oh,
                                       corner_radius_ + inflate);
        } else {
            canvas.stroke_rect(ox, oy, ow, oh);
        }

        if (outline_style_ == BorderStyle::dashed
                || outline_style_ == BorderStyle::dotted) {
            canvas.set_line_dash(nullptr, 0, 0.0f);
        }
    }

    // Focus ring — only show on text input widgets, not sliders/toggles/meters
    // Matches CSS :focus-visible behavior (keyboard focus on text inputs)
    if (has_focus_ && focusable_ &&
        access_role_ != AccessRole::slider &&
        access_role_ != AccessRole::toggle &&
        access_role_ != AccessRole::meter) {
        // TextEditor handles its own focus border, so skip the generic ring
        // for views that paint their own focus state
    }

    // End compositing layer (restore pops the saveLayer, compositing the subtree)
    if (needs_layer)
        canvas.restore();

    // End backdrop-filter layer (issue-926). Composites the widget's own
    // opacity layer over the blurred parent backdrop.
    if (needs_backdrop_layer)
        canvas.restore();

    canvas.restore();
}

void View::simulate_click(Point root_pos) {
    auto* target = hit_test(root_pos);
    if (!target) return;

    // Convert to target's local coordinates
    Point local = root_pos;
    View* v = target;
    while (v && v != this) {
        local.x -= v->bounds().x;
        local.y -= v->bounds().y;
        v = v->parent();
    }

    target->on_mouse_down(local);
    target->on_mouse_up(local);
    // pulp #1067 — DOM-style click bubbling. `hit_test` returns the deepest
    // hit-testable view, which for `<button onClick=...>Label</button>` is
    // the inner Label child. Walk up the parent chain to find the nearest
    // ancestor with a registered handler. Mirrors the same bubble pass the
    // mac mouseUp path performs (see core/view/platform/mac/window_host_mac.mm).
    //
    // pulp #1171 (Codex P2 on #1073) — bound the bubble walk to `this`
    // (inclusive). Walking past the receiver into ancestors outside its
    // subtree leaks synthetic clicks across component boundaries — a
    // false-positive hazard for tests / tooling that simulate
    // interaction on isolated subtrees.
    View* click_target = target;
    while (click_target && !click_target->on_click) {
        if (click_target == this) break;  // stop at receiver, even if no handler
        click_target = click_target->parent();
    }
    if (click_target && click_target->on_click) click_target->on_click();
}

void View::simulate_drag(Point start, Point end, int steps) {
    auto* target = hit_test(start);
    if (!target) return;

    target->on_mouse_down(start);
    for (int i = 1; i <= steps; ++i) {
        float t = static_cast<float>(i) / steps;
        Point p = {start.x + (end.x - start.x) * t,
                   start.y + (end.y - start.y) * t};
        target->on_mouse_drag(p);
    }
    target->on_mouse_up(end);
}

static void collect_focusable(View& root, std::vector<View*>& out) {
    if (root.focusable()) out.push_back(&root);
    for (size_t i = 0; i < root.child_count(); ++i)
        collect_focusable(*root.child_at(i), out);
}

View* View::focus_next(View& root, View* current) {
    std::vector<View*> focusable;
    collect_focusable(root, focusable);
    if (focusable.empty()) return nullptr;

    if (!current) {
        focusable[0]->set_focus(true);
        return focusable[0];
    }

    current->set_focus(false);
    for (size_t i = 0; i < focusable.size(); ++i) {
        if (focusable[i] == current) {
            auto* next = focusable[(i + 1) % focusable.size()];
            next->set_focus(true);
            return next;
        }
    }
    focusable[0]->set_focus(true);
    return focusable[0];
}

View* View::focus_prev(View& root, View* current) {
    std::vector<View*> focusable;
    collect_focusable(root, focusable);
    if (focusable.empty()) return nullptr;

    if (!current) {
        focusable.back()->set_focus(true);
        return focusable.back();
    }

    current->set_focus(false);
    for (size_t i = 0; i < focusable.size(); ++i) {
        if (focusable[i] == current) {
            auto* prev = focusable[(i + focusable.size() - 1) % focusable.size()];
            prev->set_focus(true);
            return prev;
        }
    }
    focusable.back()->set_focus(true);
    return focusable.back();
}

void View::set_bounds(Rect r) {
    if (bounds_ == r) return;
    bounds_ = r;
    on_resized();
}

void View::set_window_host(WindowHost* host) {
    window_host_ = host;
    for (auto& child : children_) {
        child->set_window_host(host);
    }
}

void View::set_plugin_view_host(PluginViewHost* host) {
    plugin_view_host_ = host;
    for (auto& child : children_) {
        child->set_plugin_view_host(host);
    }
}

void View::add_child(std::unique_ptr<View> child) {
    child->parent_ = this;
    child->set_window_host(window_host_);
    child->set_plugin_view_host(plugin_view_host_);
    children_.push_back(std::move(child));
    children_.back()->on_attached();
}

std::unique_ptr<View> View::remove_child(View* child) {
    auto it = std::find_if(children_.begin(), children_.end(),
        [child](const auto& p) { return p.get() == child; });
    if (it == children_.end()) return nullptr;

    child->on_detached();
    child->set_window_host(nullptr);
    child->set_plugin_view_host(nullptr);
    child->parent_ = nullptr;
    auto owned = std::move(*it);
    children_.erase(it);
    return owned;
}

std::vector<View*> View::sorted_children_by_z_index() const {
    std::vector<View*> result;
    result.reserve(children_.size());
    for (const auto& child : children_) result.push_back(child.get());
    // Stable sort so siblings with equal z_index() retain insertion
    // order (CSS painting-order rule, pulp #972).
    std::stable_sort(result.begin(), result.end(),
        [](const View* a, const View* b) {
            return a->z_index() < b->z_index();
        });
    return result;
}

View* View::hit_test(Point local_point) {
    if (!visible_ || !enabled_ || !hit_testable_) return nullptr;

    // React Native pointerEvents (issue-1026):
    //   none      — neither this view nor children intercept events.
    //   box_none  — this view is invisible to hit-testing but children
    //               can still receive events (descend, but never return self).
    //   box_only  — this view receives events; children do NOT
    //               (skip the descent below, then check own bounds).
    //   auto_     — default behavior.
    if (pointer_events_ == PointerEvents::none) return nullptr;

    // Check children topmost-first (pulp #972). With z-index honored,
    // "topmost" means highest z_index — and at equal z, latest insertion
    // — so iterate the z-sorted paint order in reverse. Without this,
    // a high-z popover could render on top yet have clicks fall through
    // to siblings beneath it.
    if (pointer_events_ != PointerEvents::box_only) {
        auto paint_order = sorted_children_by_z_index();
        for (auto it = paint_order.rbegin(); it != paint_order.rend(); ++it) {
            View* child = *it;
            if (!child->visible_) continue;

            Point child_point = {local_point.x - child->bounds_.x,
                                local_point.y - child->bounds_.y};

            // For overflow:visible, expand the hit area on all four sides
            // to include content that extends beyond the child's bounds
            // (e.g. dropdowns/popovers that grow downward, leftward, etc.).
            // The 500px slack is symmetric so left-extending popovers
            // (e.g. the Spectr bands picker) get hit-tested correctly —
            // see pulp #1148 for the original right/down-only asymmetry.
            bool in_bounds = child->local_bounds().contains(child_point);
            if (!in_bounds && child->overflow() == Overflow::visible) {
                auto lb = child->local_bounds();
                in_bounds = child_point.x >= lb.x - 500 &&
                            child_point.x <= lb.x + lb.width + 500 &&
                            child_point.y >= lb.y - 500 &&
                            child_point.y <= lb.y + lb.height + 500;
            }

            if (in_bounds) {
                auto* hit = child->hit_test(child_point);
                if (hit) return hit;
            }
        }
    }

    // No child was hit — return this view if the point is within bounds.
    // box_none suppresses self-targeting even when a child miss falls back
    // here, matching RN's "container is just a layout pass-through" mode.
    if (pointer_events_ == PointerEvents::box_none) return nullptr;

    if (local_bounds().contains(local_point))
        return this;

    return nullptr;
}

// ── Overlay paint queue ──────────────────────────────────────────────────────

std::vector<View::OverlayRequest>& View::overlay_queue() {
    static std::vector<OverlayRequest> queue;
    return queue;
}

// Inspector hooks — set by the inspector module via function pointers
// to avoid circular dependency (view → inspect).
static std::function<void(canvas::Canvas&)> s_inspector_paint_hook;
static std::function<bool(const KeyEvent&)> s_inspector_key_hook;
static std::function<bool(const MouseEvent&)> s_inspector_mouse_hook;

void View::set_inspector_paint_hook(std::function<void(canvas::Canvas&)> hook) {
    s_inspector_paint_hook = std::move(hook);
}
void View::set_inspector_key_hook(std::function<bool(const KeyEvent&)> hook) {
    s_inspector_key_hook = std::move(hook);
}
void View::set_inspector_mouse_hook(std::function<bool(const MouseEvent&)> hook) {
    s_inspector_mouse_hook = std::move(hook);
}
bool View::call_inspector_key_hook(const KeyEvent& e) {
    return s_inspector_key_hook ? s_inspector_key_hook(e) : false;
}
bool View::call_inspector_mouse_hook(const MouseEvent& e) {
    return s_inspector_mouse_hook ? s_inspector_mouse_hook(e) : false;
}

// pulp #1148 — generalized overlay-click routing.
View* View::active_overlay_ = nullptr;

// pulp #1708 — global input-focus slot. Auto-cleared by ~View() when the
// focused widget is destroyed, preventing use-after-free in the platform
// window host's keyDown handler.
View* View::focused_input_ = nullptr;

// pulp #1361 — dismiss-path release. Pulls the slot, then fires the
// dismissed View's `on_overlay_dismissed` callback so React state can
// sync. Order matters: clear the slot FIRST so a callback that calls
// claim_overlay() on a replacement popover doesn't immediately get
// nulled out by our subsequent clear.
void View::dismiss_active_overlay() {
    View* victim = active_overlay_;
    if (!victim) return;
    active_overlay_ = nullptr;
    if (victim->on_overlay_dismissed) {
        victim->on_overlay_dismissed();
    }
}

namespace {

// pulp #1320 — recursively expand a child's painted-bounds contribution
// up through any `overflow:visible` descendants. Returns the bounding
// rect (in window coords) of `v` and every transitive descendant whose
// chain back to `v` is entirely overflow:visible. A descendant inside
// an `overflow:hidden` ancestor is clipped, so it stops contributing.
//
// `parent_abs_x` / `parent_abs_y` are the absolute window-coord origin
// of `v->parent()`. The function consumes those, applies `v`'s own
// `bounds().x/y`, and recurses.
void accumulate_overflow_extent(const View* v,
                                float parent_abs_x,
                                float parent_abs_y,
                                float& min_x,
                                float& min_y,
                                float& max_x,
                                float& max_y) {
    if (!v) return;
    const float abs_x = parent_abs_x + v->bounds().x;
    const float abs_y = parent_abs_y + v->bounds().y;
    const auto lb = v->local_bounds();
    if (abs_x < min_x) min_x = abs_x;
    if (abs_y < min_y) min_y = abs_y;
    if (abs_x + lb.width > max_x) max_x = abs_x + lb.width;
    if (abs_y + lb.height > max_y) max_y = abs_y + lb.height;
    // Only recurse through children whose own overflow is visible —
    // that's the CSS rule. An `overflow:hidden` child clips its own
    // descendants, so they don't contribute painted pixels above us.
    if (v->overflow() != View::Overflow::visible) return;
    for (size_t i = 0; i < v->child_count(); ++i) {
        accumulate_overflow_extent(v->child_at(i), abs_x, abs_y,
                                   min_x, min_y, max_x, max_y);
    }
}

}  // namespace

bool View::overlay_contains(Point window_pt) const {
    // Walk up to compute absolute origin in window/root coords. Same
    // arithmetic the mac window-host uses for ComboBox::active_popup_.
    float abs_x = 0.0f, abs_y = 0.0f;
    const View* v = this;
    while (v) {
        abs_x += v->bounds().x;
        abs_y += v->bounds().y;
        v = v->parent();
    }
    const float w = local_bounds().width;
    const float h = local_bounds().height;
    // Fast-path: own painted rect contains the point.
    if (window_pt.x >= abs_x && window_pt.x <= abs_x + w &&
        window_pt.y >= abs_y && window_pt.y <= abs_y + h) {
        return true;
    }

    // pulp #1320 — extend the hit area to include the painted bounding
    // box of any `overflow:visible` descendants. CSS `overflow:visible`
    // semantics: a child painting outside the parent is still
    // visible/clickable. Without this, a popover positioned via
    // `position:absolute; top: 28; right: 0` extends LEFTWARD beyond
    // its short trigger button — clicks on the leftward cells then
    // miss `overlay_contains` and fall through to whatever sibling
    // happens to occupy that pixel.
    //
    // Only meaningful when this overlay itself has overflow:visible
    // (otherwise its own clip rect bounds the painted pixels).
    if (overflow() != Overflow::visible) return false;

    // Compute parent_abs_{x,y}: this->bounds().x/y were already added
    // by the walk above, so subtract them to get the parent origin.
    const float parent_abs_x = abs_x - bounds().x;
    const float parent_abs_y = abs_y - bounds().y;
    float min_x = abs_x, min_y = abs_y;
    float max_x = abs_x + w, max_y = abs_y + h;
    accumulate_overflow_extent(this, parent_abs_x, parent_abs_y,
                               min_x, min_y, max_x, max_y);
    return window_pt.x >= min_x && window_pt.x <= max_x &&
           window_pt.y >= min_y && window_pt.y <= max_y;
}

void View::paint_overlays(canvas::Canvas& canvas) {
    auto& queue = overlay_queue();
    for (auto& req : queue) {
        if (req.paint_fn) req.paint_fn(canvas);
    }
    queue.clear();

    // Inspector paint hook — called after all overlays, topmost layer
    if (s_inspector_paint_hook) {
        s_inspector_paint_hook(canvas);
    }
}

Color View::resolve_color(const std::string& name, Color fallback) const {
    auto c = theme_.color(name);
    if (c.has_value()) return c.value();
    if (parent_) return parent_->resolve_color(name, fallback);
    return fallback;
}

float View::resolve_dimension(const std::string& name, float fallback) const {
    auto d = theme_.dimension(name);
    if (d.has_value()) return d.value();
    if (parent_) return parent_->resolve_dimension(name, fallback);
    return fallback;
}

// ── CSS-style typography inheritance (issue-969) ────────────────────────
//
// Each inheritable_*() walks the chain own → parent → … → root, returning
// the first ancestor that has a value. nullopt means no one in the chain
// set the field, so the caller falls back to the theme/widget default.

std::optional<Color> View::inheritable_text_color() const {
    if (inh_text_color_.has_value()) return inh_text_color_;
    if (parent_) return parent_->inheritable_text_color();
    return std::nullopt;
}

std::optional<float> View::inheritable_font_size() const {
    if (inh_font_size_.has_value()) return inh_font_size_;
    if (parent_) return parent_->inheritable_font_size();
    return std::nullopt;
}

std::optional<float> View::inheritable_letter_spacing() const {
    if (inh_letter_spacing_.has_value()) return inh_letter_spacing_;
    if (parent_) return parent_->inheritable_letter_spacing();
    return std::nullopt;
}

std::optional<int> View::inheritable_font_weight() const {
    if (inh_font_weight_.has_value()) return inh_font_weight_;
    if (parent_) return parent_->inheritable_font_weight();
    return std::nullopt;
}

std::optional<std::string> View::inheritable_font_family() const {
    if (inh_font_family_.has_value()) return inh_font_family_;
    if (parent_) return parent_->inheritable_font_family();
    return std::nullopt;
}

std::optional<int> View::inheritable_text_align() const {
    if (inh_text_align_.has_value()) return inh_text_align_;
    if (parent_) return parent_->inheritable_text_align();
    return std::nullopt;
}

// ── Pointer capture ─────────────────────────────────────────────────────

void View::set_pointer_capture(int pointer_id) {
    if (!has_pointer_capture(pointer_id))
        captured_pointers_.push_back(pointer_id);
}

void View::release_pointer_capture(int pointer_id) {
    auto it = std::find(captured_pointers_.begin(), captured_pointers_.end(), pointer_id);
    if (it != captured_pointers_.end())
        captured_pointers_.erase(it);
}

bool View::has_pointer_capture(int pointer_id) const {
    return std::find(captured_pointers_.begin(), captured_pointers_.end(), pointer_id)
           != captured_pointers_.end();
}

// ── Hover ───────────────────────────────────────────────────────────────

void View::set_hovered(bool h) {
    if (hovered_ == h) return;
    hovered_ = h;
    if (h) {
        on_mouse_enter();
        if (on_hover_enter) on_hover_enter();
    } else {
        on_mouse_leave();
        if (on_hover_leave) on_hover_leave();
    }
}

FrameClock* View::frame_clock() const {
    if (frame_clock_) return frame_clock_;
    if (parent_) return parent_->frame_clock();
    return nullptr;
}

void View::request_repaint() {
    // set_window_host / set_plugin_view_host propagate to children on
    // add_child, so any attached view sees its own host pointer and we
    // never need to walk the parent chain. No host attached: silent
    // no-op — paint is already on the way for the initial mount, or
    // there's no surface to paint to yet.
    if (window_host_) {
        window_host_->repaint();
    } else if (plugin_view_host_) {
        plugin_view_host_->repaint();
    }
}

void View::simulate_hover(Point root_pos) {
    // Clear hover on all children first via a simple recursive walk
    std::function<void(View*)> clear_hover = [&](View* v) {
        if (v->hovered_) v->set_hovered(false);
        for (size_t i = 0; i < v->child_count(); ++i)
            clear_hover(v->child_at(i));
    };
    clear_hover(this);

    // Set hover on the hit target
    auto* target = hit_test(root_pos);
    if (target) target->set_hovered(true);
}

// ── Grid template parsing ────────────────────────────────────────────────────

std::vector<GridTrack> GridStyle::parse_template(const std::string& tmpl) {
    std::vector<GridTrack> tracks;
    std::istringstream ss(tmpl);
    std::string token;
    while (ss >> token) {
        if (token.back() == 'r' && token.size() > 2 && token[token.size()-2] == 'f') {
            // "1fr", "2.5fr"
            float val = std::stof(token.substr(0, token.size() - 2));
            tracks.push_back(GridTrack::fractional(val));
        } else if (token == "auto") {
            tracks.push_back(GridTrack::auto_size());
        } else {
            // "100px" or "100" — treat as fixed pixels
            float val = std::stof(token);
            tracks.push_back(GridTrack::fixed_px(val));
        }
    }
    return tracks;
}

std::vector<GridStyle::NamedArea> GridStyle::parse_template_areas(const std::string& css) {
    // pulp #1434 Phase A2-2 — parse CSS grid-template-areas:
    //   "'header header header' 'main side side' 'footer footer footer'"
    // Each single-quoted segment is one row; cells are space-separated.
    // Adjacent cells with the same name (in the same row OR across
    // adjacent rows in the same column) merge into one rectangle.
    // `'.'` is the CSS spec spacer — skipped entirely.
    std::vector<std::vector<std::string>> rows;
    {
        std::string s = css;
        // Trim whitespace.
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(0, 1);
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
        // Walk single-quoted runs.
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == '\'') {
                size_t end = s.find('\'', i + 1);
                if (end == std::string::npos) break;
                std::string row_str = s.substr(i + 1, end - i - 1);
                std::vector<std::string> cells;
                std::istringstream iss(row_str);
                std::string tok;
                while (iss >> tok) cells.push_back(tok);
                rows.push_back(std::move(cells));
                i = end + 1;
            } else {
                ++i;
            }
        }
    }
    if (rows.empty()) return {};

    // Build a name → bounding-rect map. Each cell contributes to the
    // rectangle if it shares the name. CSS spec requires the area to
    // be rectangular; non-rectangular shapes are technically invalid
    // but we accept them as the bounding rect (lenient at the IR layer).
    std::vector<NamedArea> out;
    auto find = [&](const std::string& name) -> NamedArea* {
        for (auto& a : out) if (a.name == name) return &a;
        return nullptr;
    };
    for (size_t r = 0; r < rows.size(); ++r) {
        for (size_t c = 0; c < rows[r].size(); ++c) {
            const std::string& name = rows[r][c];
            if (name == "." || name.empty()) continue;
            int row1 = static_cast<int>(r) + 1; // CSS line numbers are 1-based
            int col1 = static_cast<int>(c) + 1;
            int row2 = row1 + 1;
            int col2 = col1 + 1;
            if (auto* existing = find(name)) {
                existing->col_start = std::min(existing->col_start, col1);
                existing->row_start = std::min(existing->row_start, row1);
                existing->col_end   = std::max(existing->col_end,   col2);
                existing->row_end   = std::max(existing->row_end,   row2);
            } else {
                out.push_back({name, col1, col2, row1, row2});
            }
        }
    }
    return out;
}

// ── Grid layout algorithm ───────────────────────────────────────────────────

static void layout_grid(View& parent) {
    auto area = parent.local_bounds();
    auto& gs = parent.grid();
    auto& fs = parent.flex();

    // Padding
    float pt = fs.padding_top >= 0 ? fs.padding_top : fs.padding;
    float pr = fs.padding_right >= 0 ? fs.padding_right : fs.padding;
    float pb = fs.padding_bottom >= 0 ? fs.padding_bottom : fs.padding;
    float pl = fs.padding_left >= 0 ? fs.padding_left : fs.padding;
    area = {area.x + pl, area.y + pt, area.width - pl - pr, area.height - pt - pb};

    auto& cols = gs.template_columns;
    auto& rows = gs.template_rows;
    float col_gap = gs.column_gap;
    float row_gap = gs.row_gap;

    if (cols.empty()) return;  // No grid definition

    // Resolve column widths
    int num_cols = static_cast<int>(cols.size());
    std::vector<float> col_widths(static_cast<size_t>(num_cols), 0);
    float total_fixed_w = 0;
    float total_fr_w = 0;
    float total_col_gaps = num_cols > 1 ? col_gap * (num_cols - 1) : 0;

    for (int i = 0; i < num_cols; ++i) {
        if (cols[static_cast<size_t>(i)].type == GridTrack::Type::fixed) {
            col_widths[static_cast<size_t>(i)] = cols[static_cast<size_t>(i)].value;
            total_fixed_w += cols[static_cast<size_t>(i)].value;
        } else if (cols[static_cast<size_t>(i)].type == GridTrack::Type::fr) {
            total_fr_w += cols[static_cast<size_t>(i)].value;
        }
    }

    float remaining_w = area.width - total_fixed_w - total_col_gaps;
    if (remaining_w < 0) remaining_w = 0;

    for (int i = 0; i < num_cols; ++i) {
        auto& t = cols[static_cast<size_t>(i)];
        if (t.type == GridTrack::Type::fr && total_fr_w > 0) {
            col_widths[static_cast<size_t>(i)] = remaining_w * (t.value / total_fr_w);
        } else if (t.type == GridTrack::Type::auto_) {
            // Auto: share remaining equally among auto tracks
            col_widths[static_cast<size_t>(i)] = remaining_w / std::max(1.0f, static_cast<float>(num_cols));
        }
    }

    // Collect visible children
    std::vector<View*> children;
    for (size_t i = 0; i < parent.child_count(); ++i) {
        auto* child = parent.child_at(i);
        if (child->visible()) children.push_back(child);
    }

    // Auto-place children in grid cells
    int num_rows_needed = rows.empty()
        ? static_cast<int>((children.size() + static_cast<size_t>(num_cols) - 1) / static_cast<size_t>(num_cols))
        : static_cast<int>(rows.size());

    // Resolve row heights
    std::vector<float> row_heights(static_cast<size_t>(num_rows_needed), 0);
    float total_fixed_h = 0;
    float total_fr_h = 0;
    float total_row_gaps = num_rows_needed > 1 ? row_gap * (num_rows_needed - 1) : 0;

    for (int i = 0; i < num_rows_needed; ++i) {
        if (i < static_cast<int>(rows.size())) {
            auto& t = rows[static_cast<size_t>(i)];
            if (t.type == GridTrack::Type::fixed) {
                row_heights[static_cast<size_t>(i)] = t.value;
                total_fixed_h += t.value;
            } else if (t.type == GridTrack::Type::fr) {
                total_fr_h += t.value;
            }
        }
    }

    float remaining_h = area.height - total_fixed_h - total_row_gaps;
    if (remaining_h < 0) remaining_h = 0;

    for (int i = 0; i < num_rows_needed; ++i) {
        if (i < static_cast<int>(rows.size())) {
            auto& t = rows[static_cast<size_t>(i)];
            if (t.type == GridTrack::Type::fr && total_fr_h > 0) {
                row_heights[static_cast<size_t>(i)] = remaining_h * (t.value / total_fr_h);
            } else if (t.type == GridTrack::Type::auto_) {
                row_heights[static_cast<size_t>(i)] = 30.0f;  // Default auto row height
            }
        } else {
            // Implicit rows (auto-generated) — use auto height
            row_heights[static_cast<size_t>(i)] = 30.0f;
        }
    }

    // Position children in cells
    for (size_t ci = 0; ci < children.size(); ++ci) {
        auto* child = children[ci];
        auto& child_grid = child->grid();

        int col, row_idx;
        if (child_grid.grid_column_start > 0) {
            col = child_grid.grid_column_start - 1;  // CSS is 1-based
        } else {
            col = static_cast<int>(ci) % num_cols;
        }
        if (child_grid.grid_row_start > 0) {
            row_idx = child_grid.grid_row_start - 1;
        } else {
            row_idx = static_cast<int>(ci) / num_cols;
        }

        if (col >= num_cols) col = num_cols - 1;
        if (row_idx >= num_rows_needed) row_idx = num_rows_needed - 1;

        // Compute position from column/row offsets
        float x = area.x;
        for (int c = 0; c < col; ++c)
            x += col_widths[static_cast<size_t>(c)] + col_gap;

        float y = area.y;
        for (int r = 0; r < row_idx; ++r)
            y += row_heights[static_cast<size_t>(r)] + row_gap;

        float w = col_widths[static_cast<size_t>(col)];
        float h = row_heights[static_cast<size_t>(row_idx)];

        // Handle column/row span
        int col_end = child_grid.grid_column_end > 0 ? child_grid.grid_column_end - 1 : col + 1;
        int row_end = child_grid.grid_row_end > 0 ? child_grid.grid_row_end - 1 : row_idx + 1;
        for (int c = col + 1; c < col_end && c < num_cols; ++c)
            w += col_widths[static_cast<size_t>(c)] + col_gap;
        for (int r = row_idx + 1; r < row_end && r < num_rows_needed; ++r)
            h += row_heights[static_cast<size_t>(r)] + row_gap;

        child->set_bounds({x, y, w, h});
        child->layout_children();
    }
}

float View::intrinsic_height() const {
    // Containers: sum visible children's heights + gaps (CSS auto height behavior)
    if (children_.empty()) return 0;

    // pulp #1434 (rn batch B) — column_reverse is still a column-axis
    // container for the auto-height calculation; only true row
    // containers skip child-summed height.
    bool is_col = (flex_.direction == FlexDirection::column ||
                   flex_.direction == FlexDirection::column_reverse);
    if (!is_col) return 0;  // Row containers don't auto-height from children

    float total = 0;
    float gap = flex_.effective_gap(flex_.direction);
    int count = 0;
    for (auto& child : children_) {
        if (!child->visible_) continue;
        auto& cf = child->flex();
        float h = cf.preferred_height;
        if (h <= 0) h = child->intrinsic_height();
        total += h + cf.margin_t() + cf.margin_b();
        if (count > 0) total += gap;
        ++count;
    }

    // Add padding
    float pt = flex_.padding_top >= 0 ? flex_.padding_top : flex_.padding;
    float pb = flex_.padding_bottom >= 0 ? flex_.padding_bottom : flex_.padding;
    return total + pt + pb;
}

#ifdef PULP_HAS_YOGA
void yoga_layout(View& root); // implemented in yoga_layout.cpp
#endif

void View::layout_children() {
    if (children_.empty()) return;

    // Dispatch to grid layout if layout mode is grid
    if (layout_mode_ == LayoutMode::grid) {
        layout_grid(*this);
        return;
    }

#ifdef PULP_HAS_YOGA
    // Use Yoga for flexbox layout (correct margin:auto, flex-wrap, absolute positioning)
    yoga_layout(*this);
    return;
#endif

    auto area = local_bounds();

    // Per-side padding
    float pt = flex_.padding_top >= 0 ? flex_.padding_top : flex_.padding;
    float pr = flex_.padding_right >= 0 ? flex_.padding_right : flex_.padding;
    float pb = flex_.padding_bottom >= 0 ? flex_.padding_bottom : flex_.padding;
    float pl = flex_.padding_left >= 0 ? flex_.padding_left : flex_.padding;
    area = {area.x + pl, area.y + pt, area.width - pl - pr, area.height - pt - pb};

    // pulp #1434 (rn batch B) — row_reverse is still a row-axis
    // container; only the visual order of children is reversed.
    bool is_row = (flex_.direction == FlexDirection::row ||
                   flex_.direction == FlexDirection::row_reverse);
    float main_size = is_row ? area.width : area.height;
    float cross_size = is_row ? area.height : area.width;
    float gap = flex_.effective_gap(flex_.direction);

    // ── Collect visible children, sorted by order ────────────────────
    struct ChildEntry { View* view; int order; };
    std::vector<ChildEntry> ordered;
    for (auto& child : children_) {
        if (!child->visible_) continue;
        ordered.push_back({child.get(), child->flex().order});
    }
    // Stable sort by order (preserves source order for equal values)
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const ChildEntry& a, const ChildEntry& b) { return a.order < b.order; });

    int visible_count = static_cast<int>(ordered.size());
    if (visible_count == 0) return;

    // ── Pass 1: Measure children (flex_basis → preferred → intrinsic) ──
    float total_fixed = 0;
    float total_flex_grow = 0;
    float total_flex_shrink = 0;
    float total_margins = 0;

    for (auto& entry : ordered) {
        auto& cf = entry.view->flex();
        // Main-axis margins
        float margin_before = is_row ? cf.margin_l() : cf.margin_t();
        float margin_after = is_row ? cf.margin_r() : cf.margin_b();
        total_margins += margin_before + margin_after;

        if (cf.flex_grow > 0) {
            total_flex_grow += cf.flex_grow;
        } else {
            float basis = cf.basis_or_preferred(is_row);
            if (basis <= 0) basis = is_row ? entry.view->intrinsic_width() : entry.view->intrinsic_height();
            float min_val = is_row ? cf.min_width : cf.min_height;
            float max_val = is_row ? cf.max_width : cf.max_height;
            float size = std::max(basis, min_val);
            if (max_val > 0) size = std::min(size, max_val);
            total_fixed += size;
            total_flex_shrink += cf.flex_shrink;
        }
    }

    float total_gaps = visible_count > 1 ? gap * (visible_count - 1) : 0;
    float remaining = main_size - total_fixed - total_gaps - total_margins;

    // ── Pass 2: Compute child sizes ───────────────────────────────────
    struct ChildLayout { View* view; float main_size; float cross_size;
                         float margin_before; float margin_after;
                         float cross_margin_before; float cross_margin_after; };
    std::vector<ChildLayout> layouts;
    layouts.reserve(static_cast<size_t>(visible_count));

    for (auto& entry : ordered) {
        auto& cf = entry.view->flex();
        float child_main;
        float mb = is_row ? cf.margin_l() : cf.margin_t();
        float ma = is_row ? cf.margin_r() : cf.margin_b();
        float cmb = is_row ? cf.margin_t() : cf.margin_l();
        float cma = is_row ? cf.margin_b() : cf.margin_r();

        if (cf.flex_grow > 0 && remaining > 0) {
            child_main = total_flex_grow > 0 ? remaining * (cf.flex_grow / total_flex_grow) : 0;
        } else if (cf.flex_grow == 0 && remaining < 0 && cf.flex_shrink > 0 && total_flex_shrink > 0) {
            float basis = cf.basis_or_preferred(is_row);
            if (basis <= 0) basis = is_row ? entry.view->intrinsic_width() : entry.view->intrinsic_height();
            float min_val = is_row ? cf.min_width : cf.min_height;
            float base = std::max(basis, min_val);
            float shrink_amount = (-remaining) * (cf.flex_shrink / total_flex_shrink);
            child_main = std::max(min_val, base - shrink_amount);
        } else {
            float basis = cf.basis_or_preferred(is_row);
            if (basis <= 0) basis = is_row ? entry.view->intrinsic_width() : entry.view->intrinsic_height();
            float min_val = is_row ? cf.min_width : cf.min_height;
            child_main = std::max(basis, min_val);
        }

        float max_main = is_row ? cf.max_width : cf.max_height;
        if (max_main > 0) child_main = std::min(child_main, max_main);

        // Cross-axis sizing — respect align_self override
        FlexAlign align = (cf.align_self != FlexAlign::auto_) ? cf.align_self : flex_.align_items;
        float cross_min = is_row ? cf.min_height : cf.min_width;
        float cross_preferred = is_row ? cf.preferred_height : cf.preferred_width;
        float cross_intrinsic = is_row ? entry.view->intrinsic_height() : entry.view->intrinsic_width();
        float cross_max = is_row ? cf.max_height : cf.max_width;
        float avail_cross = cross_size - cmb - cma;
        float child_cross;

        if (align == FlexAlign::stretch) {
            child_cross = avail_cross;
        } else {
            child_cross = cross_preferred > 0 ? cross_preferred : cross_intrinsic;
            child_cross = std::max(child_cross, cross_min);
            if (child_cross <= 0) child_cross = avail_cross;
        }
        if (cross_max > 0) child_cross = std::min(child_cross, cross_max);

        layouts.push_back({entry.view, child_main, child_cross, mb, ma, cmb, cma});
    }

    // ── Pass 3: Justify content ──────────────────────────────────────
    float total_content = 0;
    for (auto& l : layouts)
        total_content += l.main_size + l.margin_before + l.margin_after;

    float free_space = std::max(0.0f, main_size - total_content - total_gaps);
    float pos = is_row ? area.x : area.y;
    float extra_gap = 0;

    switch (flex_.justify_content) {
        case FlexJustify::start: break;
        case FlexJustify::center: pos += free_space * 0.5f; break;
        case FlexJustify::end_: pos += free_space; break;
        case FlexJustify::space_between:
            if (visible_count > 1) extra_gap = free_space / (visible_count - 1);
            break;
        case FlexJustify::space_around:
            if (visible_count > 0) {
                float around = free_space / visible_count;
                pos += around * 0.5f; extra_gap = around;
            }
            break;
        case FlexJustify::space_evenly:
            if (visible_count > 0) {
                float even = free_space / (visible_count + 1);
                pos += even; extra_gap = even;
            }
            break;
    }

    // ── Pass 4: Position children ────────────────────────────────────
    for (size_t i = 0; i < layouts.size(); ++i) {
        auto& l = layouts[i];
        auto& cf = l.view->flex();

        pos += l.margin_before;

        // Cross-axis position — respect align_self
        FlexAlign align = (cf.align_self != FlexAlign::auto_) ? cf.align_self : flex_.align_items;
        float cross_pos = (is_row ? area.y : area.x) + l.cross_margin_before;
        float avail_cross = cross_size - l.cross_margin_before - l.cross_margin_after;

        switch (align) {
            case FlexAlign::start:
            case FlexAlign::stretch:
            case FlexAlign::auto_:
                break;
            case FlexAlign::center:
                cross_pos += (avail_cross - l.cross_size) * 0.5f;
                break;
            case FlexAlign::end:
                cross_pos += avail_cross - l.cross_size;
                break;
            // pulp #1434 (rn batch B) — baseline alignment in the manual
            // (non-Yoga) layout fallback approximates as start since
            // glyph baseline metrics aren't surfaced here. Yoga's
            // YGAlignBaseline path is the correct rendering when
            // PULP_HAS_YOGA is on (the default).
            case FlexAlign::baseline:
                break;
        }

        Rect child_bounds;
        if (is_row) {
            child_bounds = {pos, cross_pos, l.main_size, l.cross_size};
        } else {
            child_bounds = {cross_pos, pos, l.cross_size, l.main_size};
        }

        l.view->set_bounds(child_bounds);
        l.view->layout_children();
        pos += l.main_size + l.margin_after + gap + extra_gap;
    }
}

} // namespace pulp::view
