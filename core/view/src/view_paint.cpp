// View paint pipeline — extracted from view.cpp (WI-27 decomposition).
//
// Holds View::paint_all and its private paint helpers (apply_canvas_transforms,
// push_effect_layers / pop_effect_layers, paint_outset_shadows,
// apply_overflow_and_clip_path, paint_background_and_border,
// paint_children_in_order, paint_post_decorations, paint_content) plus the two
// file-local corner-path builders they use. Split out of the 2400-line view.cpp
// purely to move the paint monolith into its own TU — same directory precedent
// as text_editor_paint.cpp. All definitions are View:: members declared in
// view.hpp, so the move needs no friend decls or header churn.

#include <pulp/view/view.hpp>
#include <pulp/view/tracing_badge.hpp>
#include <pulp/view/capability_fallback.hpp>
#include <pulp/canvas/canvas.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>
#include <pulp/runtime/trace.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <vector>

namespace pulp::view {

namespace {
// Build a per-corner rounded-rect path on the canvas. When any
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

// iOS "continuous" corner shape (squircle / super-ellipse approximation).
// Apple's continuousCornerShape
// extends the corner curve to ~1.528R from each side of the vertex with
// a flatter cubic profile — visually smoother than the standard
// quarter-circle rounded corner. We approximate via:
//
//   • Extension factor 1.528 — how far the curve reaches along each edge
//   • Flatter kappa 0.85 — pulls the cubic control points further from
//     the vertex, producing the gentler curvature falloff that gives
//     the squircle its characteristic appearance.
//
// For a "max radius" (R == half-axis) the extension would otherwise
// overlap into the adjacent corner. Clamp at half-axis / extension to
// prevent path self-intersection (slightly more conservative than the
// circular path's half-axis clamp, but the squircle look needs the
// elongated curve to read correctly).
void build_continuous_corner_rounded_rect_path(
    pulp::canvas::Canvas& canvas,
    float w, float h,
    float tl, float tr, float bl, float br) {
    constexpr float extension = 1.528f;
    constexpr float k         = 0.85f;
    const float half_w = w * 0.5f;
    const float half_h = h * 0.5f;
    auto clamp = [&](float r) {
        const float lim = std::min(half_w, half_h) / extension;
        return std::max(0.0f, std::min(r, lim));
    };
    tl = clamp(tl);
    tr = clamp(tr);
    bl = clamp(bl);
    br = clamp(br);
    const float etl = tl * extension;
    const float etr = tr * extension;
    const float ebl = bl * extension;
    const float ebr = br * extension;

    canvas.begin_path();
    canvas.move_to(etl, 0.0f);
    canvas.line_to(w - etr, 0.0f);
    if (etr > 0.0f)
        canvas.cubic_to(w - etr + etr * k, 0.0f,
                        w, etr - etr * k,
                        w, etr);
    canvas.line_to(w, h - ebr);
    if (ebr > 0.0f)
        canvas.cubic_to(w, h - ebr + ebr * k,
                        w - ebr + ebr * k, h,
                        w - ebr, h);
    canvas.line_to(ebl, h);
    if (ebl > 0.0f)
        canvas.cubic_to(ebl - ebl * k, h,
                        0.0f, h - ebl + ebl * k,
                        0.0f, h - ebl);
    canvas.line_to(0.0f, etl);
    if (etl > 0.0f)
        canvas.cubic_to(0.0f, etl - etl * k,
                        etl - etl * k, 0.0f,
                        etl, 0.0f);
    canvas.close_path();
}
}  // namespace

void View::apply_canvas_transforms(canvas::Canvas& canvas) {
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

        // Skew is not applied on this scalar transform path. Skew-only views
        // still enter this block but render unchanged; callers needing true CSS
        // skew should use the affine transform matrix path below.

        canvas.translate(-ox, -oy);
    }

    // Full 2D affine transform matrix. Composed onto the current canvas matrix
    // via concat_transform so parent transforms still apply and children
    // inherit. Used by setTransform(id,a,b,c,d,e,f) from JS, including
    // translateX(-50%) centering.
    //
    // transform-origin applies to the matrix path only when the caller has
    // explicitly called setTransformOrigin. setTransform() call sites that
    // never touched the origin continue to get a plain concat. When an explicit
    // origin is set, the canvas op equivalent of "transform around origin
    // (ox, oy)" is
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
}

View::EffectLayerState View::push_effect_layers(canvas::Canvas& canvas) {
    EffectLayerState state;

    // CSS `backdrop-filter: blur(N)`. A separate compositing layer
    // whose initial content is the parent surface blurred — sits BELOW the
    // widget's own opacity/filter layer so background, border, and children
    // composite over the frosted backdrop. Paired with the matching restore()
    // in pop_effect_layers.
    bool needs_backdrop_layer = (backdrop_blur() > 0.0f);
    if (needs_backdrop_layer) {
        if (!canvas.supports(canvas::CanvasCapability::backdrop_filter)) {
            warn_capability_fallback_once(
                canvas::CanvasCapability::backdrop_filter,
                "canvas backend has no backdrop-filter blur; backdrop-filter "
                "renders as an unblurred tint");
        }
        canvas.save_backdrop_filter(0, 0, bounds_.width, bounds_.height,
                                    backdrop_blur());
    }
    state.backdrop_pushed = needs_backdrop_layer;

    // Compositing layer for opacity, blur, or post-effects.
    // Both the outset box-shadow and the overflow clip must be pushed after
    // this saveLayer so that the view's own opacity / filter layer contains
    // them; otherwise CSS opacity stacking can be wrong and subsequent
    // child-layer content can be masked on some Skia paths.
    // `mix-blend-mode` forces a saveLayer the same way opacity / filter does so
    // the subtree composites back through the requested blend mode at restore()
    // time. Default `BlendMode::normal` is a paint-time no-op (kSrcOver) and
    // stays out of `needs_layer`.
    const bool needs_blend_layer = has_non_default_blend_mode();
    // CSS mask-image opens a compositing layer so the masked subtree paints
    // into an offscreen buffer that the mask shader composites against via
    // kDstIn at restore time.
    const bool needs_mask_layer = !mask_image().empty() && mask_image() != "none";
    bool needs_layer = (opacity_ < 1.0f) || (filter_blur_ > 0.0f)
                       || !filter_chain_.empty() || needs_layer_
                       || (effect_ && effect_->needs_layer())
                       || needs_blend_layer
                       || needs_mask_layer;
    // How many layers we pushed, so we pop exactly that many below. Only an
    // effect can push more than one (EffectChain pushes one per child); every
    // other path pushes a single layer.
    //
    // The effect branch is gated on effect_->needs_layer(), not merely on
    // effect_ being set: an effect that wants no layer (an empty EffectChain)
    // must not swallow the layer that opacity / blur / blend / mask still need.
    int layers_pushed = 0;
    if (needs_layer) {
        if (effect_ && effect_->needs_layer()) {
            if (!canvas.supports(canvas::CanvasCapability::sksl_post_effect)) {
                warn_capability_fallback_once(
                    canvas::CanvasCapability::sksl_post_effect,
                    "canvas backend cannot execute SkSL post-effects; view "
                    "effects render as a plain layer");
            }
            effect_->configure_layer(canvas, 0, 0, bounds_.width, bounds_.height);
            layers_pushed = effect_->layer_count();
        } else if (needs_mask_layer) {
            // CSS mask-image + mask-size composite. SkiaCanvas opens a layer
            // and queues a mask shader; restore() applies the mask via
            // SkBlendMode::kDstIn before closing. RecordingCanvas / CG /
            // fallback backends route through the default implementation,
            // which collapses to plain save_layer and bypasses the mask. The
            // mask layer takes precedence over filter / blend wraps here
            // because the mask is the outermost composite per CSS Masking
            // Module Level 1; nested filter/blend belongs inside the masked
            // content.
            if (!canvas.supports(canvas::CanvasCapability::mask_layer)) {
                warn_capability_fallback_once(
                    canvas::CanvasCapability::mask_layer,
                    "canvas backend cannot apply mask layers; mask-image "
                    "collapses to a plain layer (mask ignored)");
            }
            canvas.save_layer_with_mask(0, 0, bounds_.width, bounds_.height,
                                         opacity_, mask_image(), mask_size());
        } else if (!filter_chain_.empty()) {
            // Full CSS filter chain. Translate View::FilterOp into
            // canvas::FilterChainEntry and hand off to the canvas backend;
            // Skia composes via SkImageFilters, CG falls back to blur-only.
            if (!canvas.supports(canvas::CanvasCapability::filter_chain)) {
                warn_capability_fallback_once(
                    canvas::CanvasCapability::filter_chain,
                    "canvas backend does not honor CSS filter color ops; "
                    "filter chain collapses to blur/opacity only");
            }
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
            // saveLayer with explicit blend mode for CSS / RN `mix-blend-mode`.
            canvas.save_layer_with_blend(0, 0, bounds_.width, bounds_.height,
                                         opacity_, filter_blur_, mix_blend_mode_);
        } else {
            canvas.save_layer(0, 0, bounds_.width, bounds_.height, opacity_, filter_blur_);
        }
        // Every non-effect branch above pushes exactly one layer.
        if (layers_pushed == 0) layers_pushed = 1;
    }
    state.layers_pushed = layers_pushed;
    return state;
}

void View::pop_effect_layers(canvas::Canvas& canvas,
                             const EffectLayerState& layers) {
    // End compositing layer(s) — each restore pops one saveLayer, compositing
    // the subtree back through that layer's filter / opacity. An EffectChain
    // pushes one layer per effect, so pop what we actually pushed rather than
    // assuming one.
    for (int i = 0; i < layers.layers_pushed; ++i)
        canvas.restore();

    // End backdrop-filter layer. Composites the widget's own
    // opacity layer over the blurred parent backdrop.
    if (layers.backdrop_pushed)
        canvas.restore();
}

void View::paint_outset_shadows(canvas::Canvas& canvas) {
    // Outset drop shadows paint inside the compositing layer so the view's
    // opacity / filter / backdrop applies to them (CSS spec — shadows are
    // part of the element's stacking context). The shadow blur halo can
    // still extend past the box bounds when overflow is visible; when
    // overflow is hidden, the clip below limits the halo to the bounds
    // — same behavior browsers exhibit for clipped boxes. Inset shadows
    // paint later, on top of the content, see below.
    //
    // CSS paints a shadow list back-to-front: the FIRST layer in the
    // declaration ends up nearest the viewer, so the list is walked in
    // reverse and each layer paints over the one behind it. Order is
    // load-bearing whenever layers overlap — a soft wide halo declared
    // first must not bury the tight dark contact shadow declared after it.
    if (!shadows_.empty()) {
        const float eff_r = effective_corner_radius(bounds_.width, bounds_.height);
        for (auto it = shadows_.rbegin(); it != shadows_.rend(); ++it) {
            if (it->inset) continue;
            canvas.draw_box_shadow(0, 0, bounds_.width, bounds_.height,
                                   it->offset_x, it->offset_y,
                                   it->blur, it->spread,
                                   it->color, /*inset=*/false, eff_r);
        }
    }
}

void View::apply_overflow_and_clip_path(canvas::Canvas& canvas) {
    // Clip only when overflow:hidden / overflow:scroll is explicitly
    // opted into. Default is overflow:visible (CSS default)
    // so absolutely-positioned popover/dropdown children that extend
    // outside the parent's content bounds still paint. `scroll` clips
    // the painted box like `hidden` per CSS spec — we don't have a
    // scrollbar layer yet, but the layout-side overflow propagation
    // is wired through Yoga so descendants measure correctly.
    if (overflow_ == Overflow::hidden || overflow_ == Overflow::scroll) {
        // Marker overflow tolerance. Common imported-design pattern: an XY pad
        // or similar drag-driven widget sets overflow:hidden on the container
        // and positions a circular dot at left:cx-r, top:cy-r where (cx,cy) is
        // the value-driven center. At edge values (0 or 1) half the dot sits
        // outside the container's content bounds and gets chopped by the strict
        // CSS clip. Detect circle-markers (position:absolute, near-square
        // bounds with border-radius >= 40% of the smaller dimension) and expand
        // the clip rect just enough to admit them. Non-marker children still
        // clip normally because they don't match the circle heuristic.
        //
        // This is an import-only accommodation, so the heuristic — and its
        // O(children) per-frame scan — runs only when a container opted in via
        // set_clip_marker_tolerance(). Native/authored trees keep the strict CSS
        // `overflow:hidden` clip and pay nothing for a feature they don't use.
        float marker_pad = 0.0f;
        if (clip_marker_tolerance_) {
          for (const auto& child : children_) {
            if (!child || !child->visible_) continue;
            if (child->position_ != Position::absolute) continue;
            const auto& cb = child->bounds_;
            if (cb.width <= 0 || cb.height <= 0) continue;
            const float min_dim = std::min(cb.width, cb.height);
            const float max_dim = std::max(cb.width, cb.height);
            // Approximate-circle test: aspect close to 1, corner
            // radius close to half the smaller dim.
            const float aspect = (max_dim > 0) ? (min_dim / max_dim) : 0.0f;
            const float br = child->effective_corner_radius(cb.width, cb.height);
            if (aspect < 0.7f) continue;            // not close to square
            if (br < min_dim * 0.4f) continue;      // not visually circular
            // How far the child extends past each edge.
            const float right_over  = std::max(0.0f, cb.x + cb.width  - bounds_.width);
            const float bottom_over = std::max(0.0f, cb.y + cb.height - bounds_.height);
            const float left_over   = std::max(0.0f, -cb.x);
            const float top_over    = std::max(0.0f, -cb.y);
            marker_pad = std::max({marker_pad, right_over, bottom_over,
                                              left_over, top_over});
          }
        }
        const float clip_w = bounds_.width, clip_h = bounds_.height;
        // A uniform radius (setCornerRadius "All" -> set_border_radius) lives in
        // corner_radius_, read by effective_corner_radius; the per-corner setters
        // fill corner_radii_[]. The rounded clip has to honour BOTH, else a card
        // rounded via the common uniform path clips square. Take the larger of
        // each per-corner value and the uniform base, matching how a background
        // painted with either path rounds.
        const float uni_r = effective_corner_radius(clip_w, clip_h);
        float ctl = std::max(effective_corner_radius_tl(clip_w, clip_h), uni_r);
        float ctr = std::max(effective_corner_radius_tr(clip_w, clip_h), uni_r);
        float cbl = std::max(effective_corner_radius_bl(clip_w, clip_h), uni_r);
        float cbr = std::max(effective_corner_radius_br(clip_w, clip_h), uni_r);
        const bool rounded_clip =
            marker_pad <= 0.0f &&
            (ctl > 0.5f || ctr > 0.5f || cbl > 0.5f || cbr > 0.5f);
        if (marker_pad > 0.0f) {
            canvas.clip_rect(-marker_pad, -marker_pad,
                             bounds_.width  + 2.0f * marker_pad,
                             bounds_.height + 2.0f * marker_pad);
        } else if (rounded_clip) {
            // A rounded frame must clip to its ROUNDED box, not a square: CSS
            // overflow:hidden with border-radius clips to the rounded border box,
            // so a square clip saws the corners off (an imported card lost its
            // rounded top and bottom once it opted into clip). Clip to the same
            // rounded rect the background paints. Each radius is clamped to half
            // the shorter side so a large radius cannot self-cross the path.
            const float m = 0.5f * std::min(clip_w, clip_h);
            ctl = std::min(ctl, m); ctr = std::min(ctr, m);
            cbl = std::min(cbl, m); cbr = std::min(cbr, m);
            std::ostringstream d;
            d << "M " << ctl << " 0"
              << " H " << (clip_w - ctr) << " A " << ctr << " " << ctr << " 0 0 1 " << clip_w << " " << ctr
              << " V " << (clip_h - cbr) << " A " << cbr << " " << cbr << " 0 0 1 " << (clip_w - cbr) << " " << clip_h
              << " H " << cbl << " A " << cbl << " " << cbl << " 0 0 1 0 " << (clip_h - cbl)
              << " V " << ctl << " A " << ctl << " " << ctl << " 0 0 1 " << ctl << " 0 Z";
            if (!canvas.supports(canvas::CanvasCapability::clip_path_svg)) {
                warn_capability_fallback_once(
                    canvas::CanvasCapability::clip_path_svg,
                    "canvas backend has no SVG-path clip; rounded overflow "
                    "clip and clip-path silently do not clip");
            }
            canvas.clip_path_svg(d.str());
        } else {
            canvas.clip_rect(0, 0, bounds_.width, bounds_.height);
        }
    }

    // CSS `clip-path: path("...")`. The View's local coordinate space is
    // (0,0)→(bounds_.width, bounds_.height) at this point, so the SVG-path-d
    // string is interpreted in the border-box coordinate space. The Skia
    // backend parses via SkPath::FromSVGString and intersects the canvas clip;
    // RecordingCanvas captures a `clip_path_svg` command for tests; backends
    // without a path parser silently no-op. The clip is released by the
    // matching `canvas.restore()` at the end of paint_all; the outer
    // `canvas.save()` at function entry already covers it.
    if (!clip_path().empty()) {
        if (!canvas.supports(canvas::CanvasCapability::clip_path_svg)) {
            warn_capability_fallback_once(
                canvas::CanvasCapability::clip_path_svg,
                "canvas backend has no SVG-path clip; rounded overflow "
                "clip and clip-path silently do not clip");
        }
        canvas.clip_path_svg(clip_path());
    }
}

void View::paint_background_and_border(canvas::Canvas& canvas) {
    // Per-corner border-radius: when any of the
    // setBorderTopLeftRadius / TopRight / BottomLeft / BottomRight setters
    // has been called we paint backgrounds and the border via a path
    // approximating each corner independently. Otherwise we keep using the
    // canvas's optimized fill_rounded_rect / stroke_rounded_rect with the
    // uniform corner_radius_.
    //
    // `borderCurve: continuous` (RN iOS-style squircle) forces the path-based
    // path generator regardless of per-corner state, because the canvas's
    // optimized fill_rounded_rect uses circular corners only.
    const bool use_continuous = (border_curve_ == BorderCurve::continuous);
    const bool use_per_corner = has_corner_radii_ || use_continuous;
    // Inline dispatcher: pick the path generator (per-corner circular vs
    // continuous squircle) based on the active border-curve mode. Each
    // of the 3 path build sites below uses this same dispatch.
    auto build_corner_path = [&](float w, float h, float t1, float t2, float t3, float t4) {
        if (use_continuous) {
            build_continuous_corner_rounded_rect_path(canvas, w, h, t1, t2, t3, t4);
        } else {
            build_per_corner_rounded_rect_path(canvas, w, h, t1, t2, t3, t4);
        }
    };

    // Paint sites must read effective_* so percent slots
    // (corner_radius_pct_, corner_radii_pct_[]) resolve against the box size.
    // Reading the raw px slots makes `setBorderRadius('50%')` and per-corner
    // percent setters no-op.
    const float eff_r = effective_corner_radius(bounds_.width, bounds_.height);
    const float eff_tl = effective_corner_radius_tl(bounds_.width, bounds_.height);
    const float eff_tr = effective_corner_radius_tr(bounds_.width, bounds_.height);
    const float eff_bl = effective_corner_radius_bl(bounds_.width, bounds_.height);
    const float eff_br = effective_corner_radius_br(bounds_.width, bounds_.height);

    // Paint background gradient if set (CSS background: linear/radial/conic).
    // The canvas + Skia/CoreGraphics backends implement all three; the View
    // just dispatches on the stored type. cx/cy are box fractions; radial
    // radius is a fraction of the larger box dimension; conic angle is radians.
    if (bg_gradient_type_ > 0 && !bg_gradient_colors_.empty()) {
        const int grad_n = static_cast<int>(bg_gradient_colors_.size());
        const Color* grad_c = bg_gradient_colors_.data();
        const float* grad_p = bg_gradient_positions_.data();
        if (bg_gradient_type_ == 2) {  // radial
            canvas.set_fill_gradient_radial(
                bg_grad_x0_ * bounds_.width, bg_grad_y0_ * bounds_.height,
                bg_grad_radius_ * std::max(bounds_.width, bounds_.height),
                grad_c, grad_p, grad_n);
        } else if (bg_gradient_type_ == 3) {  // conic / sweep
            canvas.set_fill_gradient_conic(
                bg_grad_x0_ * bounds_.width, bg_grad_y0_ * bounds_.height,
                bg_grad_angle_, grad_c, grad_p, grad_n);
        } else {  // linear
            canvas.set_fill_gradient_linear(
                bg_grad_x0_ * bounds_.width, bg_grad_y0_ * bounds_.height,
                bg_grad_x1_ * bounds_.width, bg_grad_y1_ * bounds_.height,
                grad_c, grad_p, grad_n);
        }
        if (use_per_corner) {
            build_corner_path(bounds_.width, bounds_.height,
                                               eff_tl, eff_tr, eff_bl, eff_br);
            canvas.fill_current_path();
        } else if (eff_r > 0) {
            canvas.fill_rounded_rect(0, 0, bounds_.width, bounds_.height, eff_r);
        } else {
            canvas.fill_rect(0, 0, bounds_.width, bounds_.height);
        }
        canvas.clear_fill_gradient();
    }

    // Paint background if set
    if (has_bg_ && bg_gradient_type_ == 0) {
        canvas.set_fill_color(bg_color_);
        if (use_per_corner) {
            build_corner_path(bounds_.width, bounds_.height,
                                               eff_tl, eff_tr, eff_bl, eff_br);
            canvas.fill_current_path();
        } else if (eff_r > 0) {
            canvas.fill_rounded_rect(0, 0, bounds_.width, bounds_.height, eff_r);
        } else {
            canvas.fill_rect(0, 0, bounds_.width, bounds_.height);
        }
    }

    // Paint border if set. border-style is honored at paint time:
    // `none` / `hidden` short-circuit; `dashed` / `dotted` install a
    // SkDashPathEffect via canvas.set_line_dash(...) before stroking.
    // Other named styles (`double` / `groove` / `ridge` / `inset` /
    // `outset`) currently degrade to solid.
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
            build_corner_path(bounds_.width, bounds_.height,
                                               eff_tl, eff_tr, eff_bl, eff_br);
            canvas.stroke_current_path();
        } else if (eff_r > 0) {
            canvas.stroke_rounded_rect(0, 0, bounds_.width, bounds_.height, eff_r);
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
}

void View::paint_children_in_order(canvas::Canvas& canvas) {
    // Paint children. CSS z-index ordering: stable-sort
    // ascending by z_index() so siblings with equal z keep insertion
    // order (CSS painting-order rule). Higher z paints later, ending
    // up visually on top. The default z_index_ is 0, so views that never call
    // set_z_index() retain insertion order.
    // Fast path: when children are already in non-decreasing z_index order
    // (the dominant case — the default z_index_ is 0, so any tree that never
    // calls set_z_index() qualifies), a stable_sort by z is the identity, so we
    // can paint children_ in place. This avoids allocating a fresh sorted
    // vector for every interior view on every frame — paint_all runs inside a
    // ScopedNoAlloc region, so that per-frame allocation is a real-time-safety
    // violation. Only fall back to the allocating sorted copy when z-index
    // actually reorders siblings.
    if (children_in_z_order()) {
        for (const auto& child : children_) {
            child->paint_all(canvas);
        }
    } else {
        auto paint_order = sorted_children_by_z_index();
        for (View* child : paint_order) {
            child->paint_all(canvas);
        }
    }
}

void View::paint_post_decorations(canvas::Canvas& canvas,
                                  const EffectLayerState& layers) {
    // eff_r is a pure function of bounds_ + corner state (paint does not mutate
    // either), so it is identical to the value paint_background_and_border
    // computes; recomputed here so the inset shadows and outline read the same
    // radius they read in the monolithic paint_all.
    const float eff_r = effective_corner_radius(bounds_.width, bounds_.height);

    // Inset box shadows paint over the content so the inner darkening
    // shows through children (CSS spec: inset shadows are above the
    // background but below the border-image, here approximated as above
    // children too).
    // Reverse order for the same reason as the outset pass above.
    for (auto it = shadows_.rbegin(); it != shadows_.rend(); ++it) {
        if (!it->inset) continue;
        canvas.draw_box_shadow(0, 0, bounds_.width, bounds_.height,
                               it->offset_x, it->offset_y,
                               it->blur, it->spread,
                               it->color, /*inset=*/true,
                               eff_r);
    }

    // CSS / RN outline. Paints OUTSIDE the border-box and
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
        if (eff_r > 0) {
            canvas.stroke_rounded_rect(ox, oy, ow, oh,
                                       eff_r + inflate);
        } else {
            canvas.stroke_rect(ox, oy, ow, oh);
        }

        if (outline_style_ == BorderStyle::dashed
                || outline_style_ == BorderStyle::dotted) {
            canvas.set_line_dash(nullptr, 0, 0.0f);
        }
    }

    // No generic focus ring is painted here; text-capable widgets provide
    // their own focus affordance.
    if (has_focus_ && focusable_ &&
        access_role_ != AccessRole::slider &&
        access_role_ != AccessRole::toggle &&
        access_role_ != AccessRole::meter) {
        // Intentionally empty: TextEditor handles its own focus border, so
        // skip the generic ring.
    }

    // Effect overlay — painted on TOP of the subtree but INSIDE the effect's
    // compositing layer(s), so it composites as part of the effect (e.g. a
    // vignette's radial-gradient edge darkening). Only meaningful when the
    // effect actually pushed a layer.
    //
    // LIMITATION (revisit when overlays-in-chains matter): this draws into the
    // INNERMOST (top) still-open layer — correct for the single-effect path
    // (the only non-chain path today). In a multi-effect EffectChain like
    // [vignette, blur], the layers stack vignette→blur, so the vignette's
    // overlay would draw into the blur layer and get blurred. Wiring an overlay
    // to composite into a SPECIFIC effect's layer (pop-interleaved with the
    // overlay draws) is the fix if/when that combination is needed.
    if (effect_ && layers.layers_pushed > 0)
        effect_->paint_overlay(canvas, 0, 0, bounds_.width, bounds_.height);
}

void View::paint_content(canvas::Canvas& canvas, const EffectLayerState& layers,
                         std::int64_t& children_ns) {
    // The subtree's own content, painted INSIDE the effect/opacity layers that
    // push_effect_layers opened. Kept as one unit so FU-3's subtree cache can
    // wrap exactly this (background/border/paint()/children/decorations) while
    // the animatable layer wrappers stay outside the cache.
    paint_outset_shadows(canvas);
    apply_overflow_and_clip_path(canvas);
    paint_background_and_border(canvas);

    // Widget-specific painting. The outer timer wraps the whole paint_all body,
    // so `paint(canvas)` no-op overrides on styled containers still get
    // accurate self-time attribution.
    paint(canvas);

    // Time only the recursive child paint so self_ns = outer - children in
    // paint_all correctly attributes framework drawing to this view.
    auto children_t0 = std::chrono::steady_clock::now();
    paint_children_in_order(canvas);
    children_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now() - children_t0).count();

    paint_post_decorations(canvas, layers);
}

void View::paint_content_maybe_cached(canvas::Canvas& canvas,
                                      const EffectLayerState& layers,
                                      std::int64_t& children_ns) {
    // Not caching, or a backend that cannot record (CoreGraphics,
    // RecordingCanvas, base): paint the subtree directly, every frame. This is
    // the honest fallback — nothing blanks on a non-scene_cache backend.
    if (!subtree_cached_ ||
        !canvas.supports(canvas::CanvasCapability::scene_cache)) {
        paint_content(canvas, layers, children_ns);
        return;
    }

    if (!scene_cache_valid_) {
        // Cache MISS: record the subtree once. Recording re-walks the tree and
        // legitimately allocates (the SkPicture command buffer, per-view
        // corner-path strings, gradient marshalling). That is a non-realtime
        // event by definition — it replaces the N allocating frames that would
        // otherwise re-walk — so suspend the paint no-alloc contract for exactly
        // this record via ScopedAllocAllowed (narrow, cache-miss-only). The
        // lambda paints paint_content INTO the recording canvas; children_ns is
        // filled through the captured reference so self-time attribution still
        // works on the miss frame.
        pulp::runtime::ScopedAllocAllowed alloc_ok;
        scene_cache_ = canvas.record_scene(
            bounds_.width, bounds_.height,
            [this, &layers, &children_ns](canvas::Canvas& rec) {
                paint_content(rec, layers, children_ns);
            });
        scene_cache_valid_ = (scene_cache_ != nullptr);
    }

    // Replay the recording. On a cache HIT children are NOT re-walked, so
    // children_ns stays 0 and the whole replay is attributed as self-time
    // (which it is — one drawPicture). If replay is refused (foreign recording,
    // defensive), fall back to a direct paint so the frame is never blank.
    if (scene_cache_valid_ && scene_cache_ && canvas.draw_scene(*scene_cache_))
        return;
    paint_content(canvas, layers, children_ns);
}

void View::paint_all(canvas::Canvas& canvas) {
    if (!visible_) return;

    // Treat paint like the audio thread. Any allocation inside this scope is a
    // real-time-safety bug. The guard is a thread-local counter in debug builds
    // and compiles away under NDEBUG; sanitizer / debug-allocator hooks read
    // pulp::runtime::is_in_no_alloc_scope() to detect violations.
    pulp::runtime::ScopedNoAlloc no_alloc_guard;

    // Time the whole paint_all body: background, border, gradients, clipping,
    // shadows, layer setup, the paint() override, inset shadows, outlines, and
    // layer restores. With the outer-scope timer,
    // self_ns = (outer total) - (children total), which correctly attributes
    // framework drawing to the view.
    auto outer_t0 = std::chrono::steady_clock::now();
    std::int64_t children_ns = 0;

    canvas.save();
    canvas.translate(bounds_.x, bounds_.y);

    // Disabled state: reduce opacity (CSS :disabled equivalent)
    if (!enabled_) canvas.set_opacity(0.4f);

    apply_canvas_transforms(canvas);

    // Open the backdrop + effect/opacity/filter/mask/blend compositing layers.
    // The returned state is the EXACT imbalance pop_effect_layers must unwind —
    // the one deliberate save-depth asymmetry, owned by this greppable pair.
    const EffectLayerState layers = push_effect_layers(canvas);

    // Everything painted INSIDE those layers (the FU-3 subtree-cache unit).
    // Routed through the cache wrapper: when set_subtree_cached(true) and the
    // backend records, this replays a recorded scene instead of re-walking the
    // subtree; otherwise it is a straight-through paint_content call. The
    // effect/opacity layers above stay LIVE (outside the cache) so animating
    // them never re-records.
    paint_content_maybe_cached(canvas, layers, children_ns);

    pop_effect_layers(canvas, layers);

    // Always-visible tracing reminder. In a PULP_TRACING=ON build the root View
    // stamps a small "◉ TRACING" corner pill on every frame so a developer can
    // never forget that Perfetto tracing is compiled into this binary while
    // looking at the plugin UI. `if constexpr (kTracingEnabled)` discards the
    // entire block in the default OFF build: no branch, no symbols, no per-frame
    // cost. Drawn here — after the view's own compositing-layer restores but
    // before the outer restore — so it lands on top of the whole subtree, is not
    // dimmed by any root-level opacity/filter layer, and is still positioned in
    // the root's local coordinate space. The literal label is 11 UTF-8 bytes,
    // so the temporary std::string the canvas API builds stays in SSO and never
    // heap-allocates inside the paint_all no-alloc scope. Only the root paints
    // it (parent_ == nullptr); a golden-screenshot harness can suppress it via
    // set_tracing_badge_visible(false).
    if constexpr (pulp::runtime::kTracingEnabled) {
        if (parent_ == nullptr && tracing_badge_should_paint()) {
            constexpr float kFontPx = 11.0f;
            constexpr float kPadX   = 8.0f;
            constexpr float kPadY   = 4.0f;
            constexpr float kMargin = 8.0f;
            const char* const label = "◉ TRACING";
            canvas.set_font("system", kFontPx);
            const float tw     = canvas.measure_text(label);
            const float pill_w = tw + 2.0f * kPadX;
            const float pill_h = kFontPx + 2.0f * kPadY;
            const float px     = bounds_.width - pill_w - kMargin;
            const float py     = kMargin;
            // High-contrast: near-black translucent pill, bright amber glyph.
            // Fixed diagnostic-overlay colors by design — this dev-only tracing
            // badge is deliberately not themeable, so the literals are not routed
            // through resolve_color.
            canvas.set_fill_color(canvas::Color::rgba8(20, 20, 24, 220));  // token-lint:allow
            canvas.fill_rounded_rect(px, py, pill_w, pill_h, pill_h * 0.5f);
            canvas.set_fill_color(canvas::Color::rgba8(255, 190, 60, 255));  // token-lint:allow
            canvas.fill_text(label, px + kPadX, py + pill_h * 0.72f);
        }
    }

    canvas.restore();

    // Timing write-back. Outer delta covers all framework drawing + paint() +
    // inset shadows + layer restores. Subtract children to get true self-time
    // even when the view has no paint() override. Saturating cast at uint32_t
    // max (~4.29s) so a pathological frame doesn't wrap.
    auto outer_dt = std::chrono::steady_clock::now() - outer_t0;
    auto total_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(outer_dt).count();
    // children_ns was filled by paint_content (the recursive child-paint time).
    auto self_ns = total_ns - children_ns;
    if (self_ns < 0) self_ns = 0;  // defensive against clock skew on a single core
    last_paint_self_ns_ = static_cast<std::uint32_t>(
        std::min<std::int64_t>(self_ns, std::numeric_limits<std::uint32_t>::max()));
    last_paint_with_children_ns_ = static_cast<std::uint32_t>(
        std::min<std::int64_t>(total_ns, std::numeric_limits<std::uint32_t>::max()));
}

}  // namespace pulp::view
