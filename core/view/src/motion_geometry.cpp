// motion_geometry.cpp — geometry-walker + scroll-geometry helpers for
// the motion subsystem.
//
// Owns 2D affine matrix math, local→global View transforms, axis-aligned
// bounding boxes, and the resolve_geometry /
// extract_property / extract_scroll_property / property_name /
// fmt_double entry points the motion Coordinator consumes.
//
// The five entry points are declared in motion_geometry_internal.hpp;
// the file-local matrix/AABB helpers stay private to this TU.
// Relocated so geometry-walker work no longer recompiles the 2.1k-line
// motion.cpp Coordinator.

#include <pulp/view/motion.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/view.hpp>

#include "motion_geometry_internal.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <string>

namespace pulp::view::motion {


/// Column-major 2D affine. Maps (x, y) → (a·x + c·y + tx, b·x + d·y + ty).
struct Mat2D {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, tx = 0.0f, ty = 0.0f;

    static constexpr Mat2D identity() { return {}; }

    /// Right-multiply: result = lhs * rhs. Mirrors `canvas.concat_*` ops
    /// which post-multiply onto the current matrix.
    static Mat2D multiply(const Mat2D& l, const Mat2D& r) {
        return {
            l.a * r.a + l.c * r.b,
            l.b * r.a + l.d * r.b,
            l.a * r.c + l.c * r.d,
            l.b * r.c + l.d * r.d,
            l.a * r.tx + l.c * r.ty + l.tx,
            l.b * r.tx + l.d * r.ty + l.ty,
        };
    }

    static Mat2D translation(float x, float y) {
        Mat2D m; m.tx = x; m.ty = y; return m;
    }
    static Mat2D scale_uniform(float s) {
        Mat2D m; m.a = s; m.d = s; return m;
    }
    static Mat2D rotation_rad(float r) {
        const float c = std::cos(r), s = std::sin(r);
        Mat2D m;
        m.a = c; m.b = s;
        m.c = -s; m.d = c;
        return m;
    }
    /// Raw affine in (a,b,c,d,e,f) form matching canvas::concat_transform.
    static Mat2D affine(float a, float b, float c, float d, float e, float f) {
        Mat2D m; m.a = a; m.b = b; m.c = c; m.d = d; m.tx = e; m.ty = f;
        return m;
    }

    /// Apply the matrix to a 2D point.
    void apply(float x, float y, float& out_x, float& out_y) const {
        out_x = a * x + c * y + tx;
        out_y = b * x + d * y + ty;
    }
};

/// Compose this view's own paint-time transforms (translate + rotate +
/// scale around transform-origin, then the full 2D matrix around the
/// same origin when set explicitly). Excludes the leading
/// `translate(bounds.x, bounds.y)` — callers compose that separately.
Mat2D self_transform(const pulp::view::View& v) {
    Mat2D m = Mat2D::identity();
    const float w = v.bounds().width;
    const float h = v.bounds().height;
    const float ox = w * v.transform_origin_x();
    const float oy = h * v.transform_origin_y();

    const bool has_basic =
        v.scale() != 1.0f || v.rotation() != 0.0f ||
        v.translate_x() != 0.0f || v.translate_y() != 0.0f;
    if (has_basic) {
        m = Mat2D::multiply(m, Mat2D::translation(ox, oy));
        if (v.translate_x() != 0.0f || v.translate_y() != 0.0f) {
            m = Mat2D::multiply(m, Mat2D::translation(v.translate_x(),
                                                      v.translate_y()));
        }
        if (v.rotation() != 0.0f) {
            constexpr float kPi = 3.14159265358979323846f;
            m = Mat2D::multiply(m, Mat2D::rotation_rad(v.rotation() * kPi / 180.0f));
        }
        if (v.scale() != 1.0f) {
            m = Mat2D::multiply(m, Mat2D::scale_uniform(v.scale()));
        }
        m = Mat2D::multiply(m, Mat2D::translation(-ox, -oy));
    }

    if (v.has_transform_matrix()) {
        float a, b, c, d, e, f;
        v.get_transform_matrix(a, b, c, d, e, f);
        const bool apply_origin = v.transform_origin_explicit();
        if (apply_origin) m = Mat2D::multiply(m, Mat2D::translation(ox, oy));
        m = Mat2D::multiply(m, Mat2D::affine(a, b, c, d, e, f));
        if (apply_origin) m = Mat2D::multiply(m, Mat2D::translation(-ox, -oy));
    }

    return m;
}

/// Returns the parent-level paint contribution that the child sees on
/// the canvas matrix when `parent` calls `paint_all` and then paints a
/// child via `child->paint_all(canvas)`. For plain `View`, this is
/// `translate(bounds.x, bounds.y) * self_transform`. For `ScrollView`,
/// the override skips parent transforms and substitutes a scroll
/// translate, so the contribution is
/// `translate(bounds.x - scroll_x, bounds.y - scroll_y)`.
Mat2D parent_to_child_origin(const pulp::view::View& parent) {
    const auto& pb = parent.bounds();
    if (auto* sv = dynamic_cast<const pulp::view::ScrollView*>(&parent)) {
        return Mat2D::translation(pb.x - sv->scroll_x(), pb.y - sv->scroll_y());
    }
    Mat2D base = Mat2D::translation(pb.x, pb.y);
    return Mat2D::multiply(base, self_transform(parent));
}

/// Build the matrix that maps `v`'s local coords into the global frame
/// (root-relative). Walks root-down so multiplication order matches the
/// canvas: M = T(root.bounds) * X(root) * T(c1.bounds) * X(c1) * …
Mat2D local_to_global_matrix(const pulp::view::View& v,
                             bool include_self_transform) {
    std::vector<const pulp::view::View*> chain;
    for (const pulp::view::View* p = &v; p; p = p->parent()) {
        chain.push_back(p);
    }
    std::reverse(chain.begin(), chain.end());

    Mat2D m = Mat2D::identity();
    for (std::size_t i = 0; i < chain.size(); ++i) {
        const pulp::view::View* node = chain[i];
        const bool is_leaf = (i + 1 == chain.size());
        if (!is_leaf) {
            m = Mat2D::multiply(m, parent_to_child_origin(*node));
        } else {
            // Leaf: always apply its own bounds translate so we land
            // on its top-left in parent's frame, then optionally its
            // self transform.
            m = Mat2D::multiply(m, Mat2D::translation(node->bounds().x,
                                                       node->bounds().y));
            if (include_self_transform) {
                m = Mat2D::multiply(m, self_transform(*node));
            }
        }
    }
    return m;
}

/// Axis-aligned bounding box of the leaf's local rect `(0,0,w,h)` after
/// being mapped through `m`.
Rect aabb_local_through(const Mat2D& m, float w, float h) {
    float xs[4]; float ys[4];
    m.apply(0.f, 0.f, xs[0], ys[0]);
    m.apply(w,   0.f, xs[1], ys[1]);
    m.apply(0.f, h,   xs[2], ys[2]);
    m.apply(w,   h,   xs[3], ys[3]);
    float min_x = xs[0], max_x = xs[0], min_y = ys[0], max_y = ys[0];
    for (int i = 1; i < 4; ++i) {
        min_x = std::min(min_x, xs[i]);
        max_x = std::max(max_x, xs[i]);
        min_y = std::min(min_y, ys[i]);
        max_y = std::max(max_y, ys[i]);
    }
    return { min_x, min_y, max_x - min_x, max_y - min_y };
}

Rect layout_rect_in_global(const pulp::view::View& v) {
    const Mat2D m = local_to_global_matrix(v, /*include_self_transform=*/false);
    return aabb_local_through(m, v.bounds().width, v.bounds().height);
}

Rect presentation_rect_in_global(const pulp::view::View& v) {
    const Mat2D m = local_to_global_matrix(v, /*include_self_transform=*/true);
    return aabb_local_through(m, v.bounds().width, v.bounds().height);
}

Rect resolve_geometry(pulp::view::View& v,
                      GeometrySpace space,
                      GeometrySource source) {
    if (space == GeometrySpace::ViewLocal) {
        return { 0.0f, 0.0f, v.bounds().width, v.bounds().height };
    }
    // Window and Screen currently collapse onto ViewGlobal; a host that
    // exposes window-origin / screen-origin offsets can layer them here.
    return (source == GeometrySource::Presentation)
               ? presentation_rect_in_global(v)
               : layout_rect_in_global(v);
}

double extract_property(const Rect& r, GeometryProperty prop) {
    switch (prop) {
        case GeometryProperty::MinX:   return r.x;
        case GeometryProperty::MinY:   return r.y;
        case GeometryProperty::MaxX:   return r.x + r.width;
        case GeometryProperty::MaxY:   return r.y + r.height;
        case GeometryProperty::MidX:   return r.x + r.width * 0.5;
        case GeometryProperty::MidY:   return r.y + r.height * 0.5;
        case GeometryProperty::Width:  return r.width;
        case GeometryProperty::Height: return r.height;
    }
    return 0.0;
}

const char* property_name(GeometryProperty prop) {
    switch (prop) {
        case GeometryProperty::MinX:   return "minX";
        case GeometryProperty::MinY:   return "minY";
        case GeometryProperty::MaxX:   return "maxX";
        case GeometryProperty::MaxY:   return "maxY";
        case GeometryProperty::MidX:   return "midX";
        case GeometryProperty::MidY:   return "midY";
        case GeometryProperty::Width:  return "width";
        case GeometryProperty::Height: return "height";
    }
    return "?";
}

// ── Scroll-geometry helpers ──────────────────────────────────────────

double extract_scroll_property(const pulp::view::ScrollView& sv,
                               ScrollProperty prop) {
    const auto& b = sv.bounds();
    const float sx = sv.scroll_x();
    const float sy = sv.scroll_y();
    const float vw = b.width;
    const float vh = b.height;
    const pulp::view::Size cs = sv.content_size();

    switch (prop) {
        case ScrollProperty::ContentOffsetX:    return sx;
        case ScrollProperty::ContentOffsetY:    return sy;
        case ScrollProperty::VisibleRectMinX:   return sx;
        case ScrollProperty::VisibleRectMinY:   return sy;
        case ScrollProperty::VisibleRectWidth:  return vw;
        case ScrollProperty::VisibleRectHeight: return vh;
        case ScrollProperty::ContentSizeWidth:  return cs.width;
        case ScrollProperty::ContentSizeHeight: return cs.height;
        // ScrollView does not currently expose per-edge content insets;
        // the four Inset* properties always report 0.0 and remain on
        // the enum so callers can wire them once insets land without
        // breaking the public API surface.
        case ScrollProperty::InsetTop:
        case ScrollProperty::InsetBottom:
        case ScrollProperty::InsetLeft:
        case ScrollProperty::InsetRight:
            return 0.0;
        case ScrollProperty::ScrollableMaxX:
            return std::max(0.0, static_cast<double>(cs.width) - vw);
        case ScrollProperty::ScrollableMaxY:
            return std::max(0.0, static_cast<double>(cs.height) - vh);
    }
    return 0.0;
}

const char* scroll_property_name(ScrollProperty prop) {
    switch (prop) {
        case ScrollProperty::ContentOffsetX:    return "contentOffsetX";
        case ScrollProperty::ContentOffsetY:    return "contentOffsetY";
        case ScrollProperty::VisibleRectMinX:   return "visibleRectMinX";
        case ScrollProperty::VisibleRectMinY:   return "visibleRectMinY";
        case ScrollProperty::VisibleRectWidth:  return "visibleRectWidth";
        case ScrollProperty::VisibleRectHeight: return "visibleRectHeight";
        case ScrollProperty::ContentSizeWidth:  return "contentSizeWidth";
        case ScrollProperty::ContentSizeHeight: return "contentSizeHeight";
        case ScrollProperty::InsetTop:          return "insetTop";
        case ScrollProperty::InsetBottom:       return "insetBottom";
        case ScrollProperty::InsetLeft:         return "insetLeft";
        case ScrollProperty::InsetRight:        return "insetRight";
        case ScrollProperty::ScrollableMaxX:    return "scrollableMaxX";
        case ScrollProperty::ScrollableMaxY:    return "scrollableMaxY";
    }
    return "?";
}

std::string fmt_double(double v, int precision) {
    std::ostringstream ss;
    ss << std::fixed << std::setprecision(std::max(0, precision)) << v;
    return ss.str();
}


}  // namespace pulp::view::motion
