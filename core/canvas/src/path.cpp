#include <pulp/canvas/path.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace pulp::canvas {

namespace {

constexpr float kPi = 3.14159265358979323846f;

/// Evaluate a cubic Bezier at t.
inline float cubic_at(float p0, float p1, float p2, float p3, float t) {
    const float mt = 1.0f - t;
    return mt * mt * mt * p0 + 3.0f * mt * mt * t * p1 +
           3.0f * mt * t * t * p2 + t * t * t * p3;
}

inline float quad_at(float p0, float p1, float p2, float t) {
    const float mt = 1.0f - t;
    return mt * mt * p0 + 2.0f * mt * t * p1 + t * t * p2;
}

/// Accumulate the exact extrema of one axis of a cubic into [lo, hi].
/// The endpoints are assumed already accumulated by the caller; this adds
/// only the interior turning points, i.e. the roots of B'(t) in (0, 1).
void accumulate_cubic_extrema(float p0, float p1, float p2, float p3,
                              float& lo, float& hi) {
    // B'(t) = a t^2 + b t + c   (dropping the common factor of 3)
    const float a = -p0 + 3.0f * p1 - 3.0f * p2 + p3;
    const float b = 2.0f * (p0 - 2.0f * p1 + p2);
    const float c = p1 - p0;

    auto consider = [&](float t) {
        if (t <= 0.0f || t >= 1.0f) return;
        const float v = cubic_at(p0, p1, p2, p3, t);
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    };

    if (std::abs(a) < 1e-9f) {
        // Degenerates to a linear derivative: b t + c = 0.
        if (std::abs(b) > 1e-9f) consider(-c / b);
        return;
    }
    const float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f) return;
    const float sq = std::sqrt(disc);
    consider((-b + sq) / (2.0f * a));
    consider((-b - sq) / (2.0f * a));
}

void accumulate_quad_extrema(float p0, float p1, float p2,
                             float& lo, float& hi) {
    // B'(t) = 0  ->  t = (p0 - p1) / (p0 - 2 p1 + p2)
    const float denom = p0 - 2.0f * p1 + p2;
    if (std::abs(denom) < 1e-9f) return;
    const float t = (p0 - p1) / denom;
    if (t <= 0.0f || t >= 1.0f) return;
    const float v = quad_at(p0, p1, p2, t);
    lo = std::min(lo, v);
    hi = std::max(hi, v);
}

/// Recursively flatten a cubic into line segments within `tol` of the curve.
void flatten_cubic(const Point2D& p0, const Point2D& p1, const Point2D& p2,
                   const Point2D& p3, float tol, int depth,
                   std::vector<Point2D>& out) {
    // Flatness metric: max distance of the two control points from the chord.
    const float dx = p3.x - p0.x;
    const float dy = p3.y - p0.y;
    float d1 = std::abs((p1.x - p3.x) * dy - (p1.y - p3.y) * dx);
    float d2 = std::abs((p2.x - p3.x) * dy - (p2.y - p3.y) * dx);
    const float dd = d1 + d2;

    if (depth >= 16 || dd * dd <= tol * (dx * dx + dy * dy)) {
        out.push_back(p3);
        return;
    }
    // de Casteljau split at t = 0.5.
    const Point2D p01{(p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f};
    const Point2D p12{(p1.x + p2.x) * 0.5f, (p1.y + p2.y) * 0.5f};
    const Point2D p23{(p2.x + p3.x) * 0.5f, (p2.y + p3.y) * 0.5f};
    const Point2D p012{(p01.x + p12.x) * 0.5f, (p01.y + p12.y) * 0.5f};
    const Point2D p123{(p12.x + p23.x) * 0.5f, (p12.y + p23.y) * 0.5f};
    const Point2D mid{(p012.x + p123.x) * 0.5f, (p012.y + p123.y) * 0.5f};

    flatten_cubic(p0, p01, p012, mid, tol, depth + 1, out);
    flatten_cubic(mid, p123, p23, p3, tol, depth + 1, out);
}

void flatten_quad(const Point2D& p0, const Point2D& p1, const Point2D& p2,
                  float tol, std::vector<Point2D>& out) {
    // Elevate the quadratic to a cubic and reuse one flattener.
    const Point2D c1{p0.x + (2.0f / 3.0f) * (p1.x - p0.x),
                   p0.y + (2.0f / 3.0f) * (p1.y - p0.y)};
    const Point2D c2{p2.x + (2.0f / 3.0f) * (p1.x - p2.x),
                   p2.y + (2.0f / 3.0f) * (p1.y - p2.y)};
    flatten_cubic(p0, c1, c2, p2, tol, 0, out);
}

}  // namespace

// ── Copy-on-write ────────────────────────────────────────────────────────

Path::Data& Path::mutable_data() {
    if (!data_) {
        data_ = std::make_shared<Data>();
    } else if (data_.use_count() > 1) {
        // Someone else is looking at this buffer — take our own copy before
        // touching it. This is the "copy" in copy-on-write, and the only place
        // a Path copy ever costs O(n).
        data_ = std::make_shared<Data>(*data_);
    }
    bounds_valid_ = false;
    return *data_;
}

bool Path::operator==(const Path& o) const {
    if (data_ == o.data_) return true;  // shares a buffer (or both empty)
    if (is_empty() && o.is_empty()) return true;
    if (!data_ || !o.data_) return false;
    return data_->verbs == o.data_->verbs && data_->points == o.data_->points;
}

// ── Building ─────────────────────────────────────────────────────────────

Path& Path::move_to(float x, float y) {
    Data& d = mutable_data();
    d.verbs.push_back(Verb::move);
    d.points.push_back({x, y});
    return *this;
}

Path& Path::line_to(float x, float y) {
    Data& d = mutable_data();
    // A line with no current point is a move — matches Canvas2D, where lineTo
    // before moveTo starts the subpath rather than dropping the point.
    d.verbs.push_back(d.verbs.empty() ? Verb::move : Verb::line);
    d.points.push_back({x, y});
    return *this;
}

Path& Path::quad_to(float cx, float cy, float x, float y) {
    if (is_empty()) move_to(cx, cy);
    Data& d = mutable_data();
    d.verbs.push_back(Verb::quad);
    d.points.push_back({cx, cy});
    d.points.push_back({x, y});
    return *this;
}

Path& Path::cubic_to(float c1x, float c1y, float c2x, float c2y,
                     float x, float y) {
    if (is_empty()) move_to(c1x, c1y);
    Data& d = mutable_data();
    d.verbs.push_back(Verb::cubic);
    d.points.push_back({c1x, c1y});
    d.points.push_back({c2x, c2y});
    d.points.push_back({x, y});
    return *this;
}

Path& Path::close() {
    if (is_empty()) return *this;
    Data& d = mutable_data();
    if (!d.verbs.empty() && d.verbs.back() == Verb::close) return *this;
    d.verbs.push_back(Verb::close);
    return *this;
}

void Path::clear() {
    if (!data_) return;
    Data& d = mutable_data();
    d.verbs.clear();
    d.points.clear();
    bounds_valid_ = false;
}

std::optional<Point2D> Path::current_point() const {
    if (!data_ || data_->points.empty()) return std::nullopt;
    return data_->points.back();
}

// ── Arcs ─────────────────────────────────────────────────────────────────

void Path::append_arc_cubics(float cx, float cy, float rx, float ry,
                             float start_angle, float sweep_angle,
                             const AffineTransform& xf) {
    if (sweep_angle == 0.0f) return;

    // Split into segments of at most 90 degrees: the cubic approximation of a
    // circular arc is only accurate for small sweeps (error grows sharply past
    // a quarter turn).
    const int segments =
        std::max(1, static_cast<int>(std::ceil(std::abs(sweep_angle) / (kPi * 0.5f))));
    const float delta = sweep_angle / static_cast<float>(segments);

    // Magic constant for the cubic approximation of a circular arc of angle d:
    //   k = 4/3 * tan(d / 4)
    const float k = (4.0f / 3.0f) * std::tan(delta * 0.25f);

    float angle = start_angle;
    for (int i = 0; i < segments; ++i) {
        const float a0 = angle;
        const float a1 = angle + delta;

        const float cos0 = std::cos(a0), sin0 = std::sin(a0);
        const float cos1 = std::cos(a1), sin1 = std::sin(a1);

        // On the unit circle, the tangent at angle a is (-sin a, cos a).
        Point2D c1{cx + rx * (cos0 - k * sin0), cy + ry * (sin0 + k * cos0)};
        Point2D c2{cx + rx * (cos1 + k * sin1), cy + ry * (sin1 - k * cos1)};
        Point2D end{cx + rx * cos1, cy + ry * sin1};

        if (!xf.is_identity()) {
            c1 = xf.transform_point(c1);
            c2 = xf.transform_point(c2);
            end = xf.transform_point(end);
        }

        cubic_to(c1.x, c1.y, c2.x, c2.y, end.x, end.y);
        angle = a1;
    }
}

void Path::append_svg_arc(Point2D from, float rx, float ry,
                          float x_axis_rotation_deg,
                          bool large_arc, bool sweep, Point2D to) {
    // Spec: zero radii mean "just draw a line".
    if (rx == 0.0f || ry == 0.0f) {
        line_to(to.x, to.y);
        return;
    }
    // Spec: coincident endpoints mean the arc is omitted entirely.
    if (from.x == to.x && from.y == to.y) return;

    rx = std::abs(rx);
    ry = std::abs(ry);
    const float phi = x_axis_rotation_deg * kPi / 180.0f;
    const float cos_phi = std::cos(phi);
    const float sin_phi = std::sin(phi);

    // Step 1: endpoint -> the primed coordinate system (midpoint at origin,
    // ellipse axis-aligned).
    const float dx2 = (from.x - to.x) * 0.5f;
    const float dy2 = (from.y - to.y) * 0.5f;
    const float x1p =  cos_phi * dx2 + sin_phi * dy2;
    const float y1p = -sin_phi * dx2 + cos_phi * dy2;

    // Step 2: the radii may be too small to span the endpoints. The spec says
    // to scale them up (uniformly) until they exactly reach — NOT to give up.
    float rx2 = rx * rx, ry2 = ry * ry;
    const float x1p2 = x1p * x1p, y1p2 = y1p * y1p;
    const float lambda = x1p2 / rx2 + y1p2 / ry2;
    if (lambda > 1.0f) {
        const float s = std::sqrt(lambda);
        rx *= s;
        ry *= s;
        rx2 = rx * rx;
        ry2 = ry * ry;
    }

    // Step 3: the center, in primed space.
    float num = rx2 * ry2 - rx2 * y1p2 - ry2 * x1p2;
    const float den = rx2 * y1p2 + ry2 * x1p2;
    if (den <= 0.0f) {  // both endpoints coincide in primed space
        line_to(to.x, to.y);
        return;
    }
    if (num < 0.0f) num = 0.0f;  // guard float noise when lambda == 1 exactly
    float coef = std::sqrt(num / den);
    if (large_arc == sweep) coef = -coef;

    const float cxp =  coef * rx * y1p / ry;
    const float cyp = -coef * ry * x1p / rx;

    // Step 4: back to user space.
    const float cx = cos_phi * cxp - sin_phi * cyp + (from.x + to.x) * 0.5f;
    const float cy = sin_phi * cxp + cos_phi * cyp + (from.y + to.y) * 0.5f;

    // Step 5: the start angle and the sweep.
    const float ux = (x1p - cxp) / rx;
    const float uy = (y1p - cyp) / ry;
    const float vx = (-x1p - cxp) / rx;
    const float vy = (-y1p - cyp) / ry;

    const float theta1 = std::atan2(uy, ux);
    float dtheta = std::atan2(ux * vy - uy * vx, ux * vx + uy * vy);

    if (!sweep && dtheta > 0.0f) dtheta -= 2.0f * kPi;
    else if (sweep && dtheta < 0.0f) dtheta += 2.0f * kPi;

    // Generate on the axis-aligned ellipse, then rotate it into place.
    append_arc_cubics(cx, cy, rx, ry, theta1, dtheta,
                      AffineTransform::rotation(phi, cx, cy));
}

Path& Path::arc_to(float x1, float y1, float x2, float y2, float radius) {
    // Canvas2D: with no current point, arcTo is just a moveTo to P1.
    auto cur = current_point();
    if (!cur) return move_to(x1, y1);

    const Point2D p0 = *cur;
    const Point2D p1{x1, y1};
    const Point2D p2{x2, y2};

    if (radius <= 0.0f) return line_to(x1, y1);

    // Unit vectors from the corner P1 back toward P0, and on toward P2.
    float v1x = p0.x - p1.x, v1y = p0.y - p1.y;
    float v2x = p2.x - p1.x, v2y = p2.y - p1.y;
    const float l1 = std::sqrt(v1x * v1x + v1y * v1y);
    const float l2 = std::sqrt(v2x * v2x + v2y * v2y);
    if (l1 < 1e-6f || l2 < 1e-6f) return line_to(x1, y1);
    v1x /= l1; v1y /= l1;
    v2x /= l2; v2y /= l2;

    const float cross = v1x * v2y - v1y * v2x;
    if (std::abs(cross) < 1e-6f) return line_to(x1, y1);  // collinear

    float dot = v1x * v2x + v1y * v2y;
    dot = std::clamp(dot, -1.0f, 1.0f);
    const float theta = std::acos(dot);  // interior angle at the corner

    // Distance from the corner to each tangent point.
    const float tan_dist = radius / std::tan(theta * 0.5f);
    if (!std::isfinite(tan_dist)) return line_to(x1, y1);

    const Point2D t1{p1.x + v1x * tan_dist, p1.y + v1y * tan_dist};
    const Point2D t2{p1.x + v2x * tan_dist, p1.y + v2y * tan_dist};

    // Center lies along the angle bisector, radius / sin(theta/2) from P1.
    float bx = v1x + v2x, by = v1y + v2y;
    const float bl = std::sqrt(bx * bx + by * by);
    if (bl < 1e-6f) return line_to(x1, y1);
    bx /= bl; by /= bl;
    const float center_dist = radius / std::sin(theta * 0.5f);
    const Point2D c{p1.x + bx * center_dist, p1.y + by * center_dist};

    line_to(t1.x, t1.y);

    const float a0 = std::atan2(t1.y - c.y, t1.x - c.x);
    const float a1 = std::atan2(t2.y - c.y, t2.x - c.x);
    float sweep = a1 - a0;
    // Take the minor arc — the tangent arc never sweeps more than half a turn.
    while (sweep > kPi) sweep -= 2.0f * kPi;
    while (sweep < -kPi) sweep += 2.0f * kPi;

    append_arc_cubics(c.x, c.y, radius, radius, a0, sweep);
    return *this;
}

// ── Shape helpers ────────────────────────────────────────────────────────

Path& Path::add_rect(float x, float y, float w, float h) {
    move_to(x, y);
    line_to(x + w, y);
    line_to(x + w, y + h);
    line_to(x, y + h);
    close();
    return *this;
}

Path& Path::add_rounded_rect(float x, float y, float w, float h, float radius) {
    return add_rounded_rect(x, y, w, h, radius, radius, radius, radius);
}

Path& Path::add_rounded_rect(float x, float y, float w, float h,
                             float tl, float tr, float br, float bl) {
    if (w <= 0.0f || h <= 0.0f) return *this;

    tl = std::max(0.0f, tl);
    tr = std::max(0.0f, tr);
    br = std::max(0.0f, br);
    bl = std::max(0.0f, bl);

    // CSS border-radius overlap rule: if adjacent radii on any side sum to
    // more than that side's length, scale ALL radii by the tightest factor so
    // the corners just touch instead of overlapping into a cusp.
    float scale = 1.0f;
    auto limit = [&](float sum, float extent) {
        if (sum > extent && sum > 0.0f)
            scale = std::min(scale, extent / sum);
    };
    limit(tl + tr, w);
    limit(bl + br, w);
    limit(tl + bl, h);
    limit(tr + br, h);
    if (scale < 1.0f) {
        tl *= scale; tr *= scale; br *= scale; bl *= scale;
    }

    const float r = x + w, b = y + h;

    move_to(x + tl, y);
    line_to(r - tr, y);
    if (tr > 0.0f) append_arc_cubics(r - tr, y + tr, tr, tr, -kPi * 0.5f, kPi * 0.5f);
    line_to(r, b - br);
    if (br > 0.0f) append_arc_cubics(r - br, b - br, br, br, 0.0f, kPi * 0.5f);
    line_to(x + bl, b);
    if (bl > 0.0f) append_arc_cubics(x + bl, b - bl, bl, bl, kPi * 0.5f, kPi * 0.5f);
    line_to(x, y + tl);
    if (tl > 0.0f) append_arc_cubics(x + tl, y + tl, tl, tl, kPi, kPi * 0.5f);
    close();
    return *this;
}

Path& Path::add_ellipse(float cx, float cy, float rx, float ry) {
    if (rx <= 0.0f || ry <= 0.0f) return *this;
    move_to(cx + rx, cy);
    append_arc_cubics(cx, cy, rx, ry, 0.0f, 2.0f * kPi);
    close();
    return *this;
}

Path& Path::add_arc(float cx, float cy, float rx, float ry,
                    float start_angle, float sweep_angle) {
    if (rx <= 0.0f || ry <= 0.0f) return *this;
    move_to(cx + rx * std::cos(start_angle), cy + ry * std::sin(start_angle));
    append_arc_cubics(cx, cy, rx, ry, start_angle, sweep_angle);
    return *this;
}

Path& Path::add_pie(float cx, float cy, float rx, float ry,
                    float start_angle, float sweep_angle) {
    if (rx <= 0.0f || ry <= 0.0f) return *this;
    move_to(cx, cy);
    line_to(cx + rx * std::cos(start_angle), cy + ry * std::sin(start_angle));
    append_arc_cubics(cx, cy, rx, ry, start_angle, sweep_angle);
    close();
    return *this;
}

Path& Path::add_polygon(const Point2D* points, size_t count) {
    if (points == nullptr || count < 2) return *this;
    move_to(points[0].x, points[0].y);
    for (size_t i = 1; i < count; ++i) line_to(points[i].x, points[i].y);
    close();
    return *this;
}

Path& Path::add_path(const Path& other, const AffineTransform& t) {
    if (other.is_empty()) return *this;

    // Self-append must read from a snapshot: mutable_data() below may
    // reallocate the very buffer we are iterating.
    if (&other == this) {
        Path copy = other;
        return add_path(copy, t);
    }

    Data& d = mutable_data();
    const Data& src = *other.data_;
    d.verbs.insert(d.verbs.end(), src.verbs.begin(), src.verbs.end());
    if (t.is_identity()) {
        d.points.insert(d.points.end(), src.points.begin(), src.points.end());
    } else {
        d.points.reserve(d.points.size() + src.points.size());
        for (const Point2D& p : src.points) d.points.push_back(t.transform_point(p));
    }
    return *this;
}

// ── Transforms ───────────────────────────────────────────────────────────

void Path::apply_transform(const AffineTransform& t) {
    if (is_empty() || t.is_identity()) return;
    Data& d = mutable_data();
    for (Point2D& p : d.points) p = t.transform_point(p);
}

Path Path::transformed(const AffineTransform& t) const {
    Path out = *this;          // O(1) — shares the buffer
    out.apply_transform(t);    // detaches only if it must actually change
    return out;
}

AffineTransform Path::transform_to_fit(float x, float y, float w, float h,
                                       bool preserve_proportions) const {
    const Rect2D b = bounds();

    // Degenerate guard. A zero-width path scaled to a non-zero width is a
    // division by zero; without this the whole path becomes NaN and silently
    // disappears from every subsequent render. A no-op is the only sane answer
    // — there is no scale factor that maps a line onto a box.
    if (b.width <= 0.0f || b.height <= 0.0f || w <= 0.0f || h <= 0.0f)
        return AffineTransform::identity();

    float sx = w / b.width;
    float sy = h / b.height;
    if (preserve_proportions) {
        sx = sy = std::min(sx, sy);
    }

    // Center the scaled bounds inside the target rect. When proportions are
    // preserved, exactly one axis binds and the other has slack; the slack is
    // split evenly. (When they are not preserved both terms are zero.)
    const float tx = x + (w - b.width * sx) * 0.5f;
    const float ty = y + (h - b.height * sy) * 0.5f;

    // Scale about the path's own bounds origin, then place it.
    return AffineTransform{sx, 0.0f, 0.0f, sy,
                           tx - b.x * sx, ty - b.y * sy};
}

void Path::scale_to_fit(float x, float y, float w, float h,
                        bool preserve_proportions) {
    apply_transform(transform_to_fit(x, y, w, h, preserve_proportions));
}

// ── Bounds ───────────────────────────────────────────────────────────────

Rect2D Path::control_bounds() const {
    if (is_empty()) return {};
    float minx = std::numeric_limits<float>::max();
    float miny = std::numeric_limits<float>::max();
    float maxx = std::numeric_limits<float>::lowest();
    float maxy = std::numeric_limits<float>::lowest();
    for (const Point2D& p : data_->points) {
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
    }
    if (minx > maxx) return {};
    return {minx, miny, maxx - minx, maxy - miny};
}

Rect2D Path::bounds() const {
    if (bounds_valid_) return bounds_cache_;
    bounds_valid_ = true;

    if (is_empty()) {
        bounds_cache_ = Rect2D{};
        return bounds_cache_;
    }

    float minx = std::numeric_limits<float>::max();
    float miny = std::numeric_limits<float>::max();
    float maxx = std::numeric_limits<float>::lowest();
    float maxy = std::numeric_limits<float>::lowest();
    bool any = false;

    auto add_point = [&](const Point2D& p) {
        minx = std::min(minx, p.x);
        maxx = std::max(maxx, p.x);
        miny = std::min(miny, p.y);
        maxy = std::max(maxy, p.y);
        any = true;
    };

    // Walk the verbs so we know each curve's START point — the extrema of a
    // cubic depend on all four of its points, and the first is the previous
    // verb's endpoint, which is not stored with the curve.
    Point2D cur{0.0f, 0.0f};
    const Data& d = *data_;
    size_t pi = 0;
    for (Verb v : d.verbs) {
        switch (v) {
            case Verb::move:
            case Verb::line: {
                cur = d.points[pi++];
                add_point(cur);
                break;
            }
            case Verb::quad: {
                const Point2D c1 = d.points[pi++];
                const Point2D end = d.points[pi++];
                add_point(cur);
                add_point(end);
                // Interior turning points only — endpoints are already in.
                accumulate_quad_extrema(cur.x, c1.x, end.x, minx, maxx);
                accumulate_quad_extrema(cur.y, c1.y, end.y, miny, maxy);
                cur = end;
                break;
            }
            case Verb::cubic: {
                const Point2D c1 = d.points[pi++];
                const Point2D c2 = d.points[pi++];
                const Point2D end = d.points[pi++];
                add_point(cur);
                add_point(end);
                accumulate_cubic_extrema(cur.x, c1.x, c2.x, end.x, minx, maxx);
                accumulate_cubic_extrema(cur.y, c1.y, c2.y, end.y, miny, maxy);
                cur = end;
                break;
            }
            case Verb::close:
                break;
        }
    }

    bounds_cache_ = any ? Rect2D{minx, miny, maxx - minx, maxy - miny} : Rect2D{};
    return bounds_cache_;
}

// ── Iteration ────────────────────────────────────────────────────────────

Path::Element Path::Iterator::operator*() const {
    Element el;
    if (!path_ || !path_->data_ || verb_ >= path_->data_->verbs.size()) return el;
    el.verb = path_->data_->verbs[verb_];
    el.count = Path::points_for(el.verb);
    el.points = el.count > 0 ? &path_->data_->points[point_] : nullptr;
    return el;
}

Path::Iterator& Path::Iterator::operator++() {
    if (!path_ || !path_->data_ || verb_ >= path_->data_->verbs.size()) return *this;
    point_ += static_cast<size_t>(Path::points_for(path_->data_->verbs[verb_]));
    ++verb_;
    return *this;
}

// ── Flattening ───────────────────────────────────────────────────────────

std::vector<Path::FlatSubpath> Path::flatten(float tolerance) const {
    std::vector<FlatSubpath> out;
    if (is_empty()) return out;
    const float tol = std::max(1e-4f, tolerance);

    FlatSubpath cur;
    auto flush = [&]() {
        if (cur.points.size() >= 2) out.push_back(std::move(cur));
        cur = FlatSubpath{};
    };

    for (Element el : *this) {
        switch (el.verb) {
            case Verb::move:
                flush();
                cur.points.push_back(el.points[0]);
                break;
            case Verb::line:
                if (cur.points.empty()) cur.points.push_back(el.points[0]);
                else cur.points.push_back(el.points[0]);
                break;
            case Verb::quad:
                if (cur.points.empty()) cur.points.push_back(el.points[0]);
                flatten_quad(cur.points.back(), el.points[0], el.points[1],
                             tol, cur.points);
                break;
            case Verb::cubic:
                if (cur.points.empty()) cur.points.push_back(el.points[0]);
                flatten_cubic(cur.points.back(), el.points[0], el.points[1],
                              el.points[2], tol, 0, cur.points);
                break;
            case Verb::close: {
                if (!cur.points.empty()) {
                    cur.closed = true;
                    const Point2D start = cur.points.front();
                    flush();
                    // A close does not end the path: subsequent verbs continue
                    // from the subpath's start point.
                    cur.points.push_back(start);
                }
                break;
            }
        }
    }
    flush();
    return out;
}

// ── Hit testing ──────────────────────────────────────────────────────────

bool Path::contains(Point2D p, FillRule rule) const {
    if (is_empty()) return false;

    // Cheap reject before doing any real work.
    const Rect2D b = bounds();
    if (p.x < b.x || p.x > b.right() || p.y < b.y || p.y > b.bottom())
        return false;

    const auto subpaths = flatten();

    int winding = 0;   // nonzero rule
    int crossings = 0; // even-odd rule

    for (const FlatSubpath& sp : subpaths) {
        const size_t n = sp.points.size();
        if (n < 2) continue;
        // Fill always treats a subpath as closed, whether or not the author
        // said `close()` — that is how filling works everywhere.
        for (size_t i = 0; i < n; ++i) {
            const Point2D& a = sp.points[i];
            const Point2D& c = sp.points[(i + 1) % n];
            if (a.y == c.y) continue;  // horizontal edges never cross a horizontal ray

            // Does the horizontal ray at p.y cross this edge? Use a half-open
            // rule [min, max) so a vertex exactly on the ray is counted once,
            // not twice.
            const bool downward = c.y > a.y;
            const float lo = downward ? a.y : c.y;
            const float hi = downward ? c.y : a.y;
            if (p.y < lo || p.y >= hi) continue;

            // x of the edge at p.y
            const float t = (p.y - a.y) / (c.y - a.y);
            const float xi = a.x + t * (c.x - a.x);
            if (xi <= p.x) continue;  // only count crossings to the right

            ++crossings;
            winding += downward ? 1 : -1;
        }
    }

    return rule == FillRule::evenodd ? (crossings % 2) != 0 : winding != 0;
}

// ── SVG ──────────────────────────────────────────────────────────────────

namespace {

/// Minimal, allocation-free scanner over an SVG path `d` string.
class SvgScanner {
public:
    explicit SvgScanner(const std::string& s) : s_(s) {}

    void skip_separators() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == ',' || c == ' ' || c == '\t' || c == '\n' || c == '\r')
                ++i_;
            else
                break;
        }
    }

    bool at_end() {
        skip_separators();
        return i_ >= s_.size();
    }

    /// Peek the next non-separator char without consuming it.
    char peek() {
        skip_separators();
        return i_ < s_.size() ? s_[i_] : '\0';
    }

    char take_command() {
        skip_separators();
        return i_ < s_.size() ? s_[i_++] : '\0';
    }

    /// True if the next token looks like a number (so we can detect the SVG
    /// "implicit repeated coordinate set" rule: `L 1 2 3 4` is two line_tos).
    bool next_is_number() {
        skip_separators();
        if (i_ >= s_.size()) return false;
        const char c = s_[i_];
        return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
    }

    /// Parse a float. Sets `ok` false if there wasn't one.
    float number(bool& ok) {
        skip_separators();
        if (i_ >= s_.size()) { ok = false; return 0.0f; }
        const char* begin = s_.c_str() + i_;
        char* end = nullptr;
        const float v = std::strtof(begin, &end);
        if (end == begin) { ok = false; return 0.0f; }
        i_ += static_cast<size_t>(end - begin);
        ok = true;
        return v;
    }

    /// SVG arc flags may be packed without separators ("a1 1 0 011 1"), so a
    /// flag is exactly one character: '0' or '1'.
    bool flag(bool& ok) {
        skip_separators();
        if (i_ >= s_.size()) { ok = false; return false; }
        const char c = s_[i_];
        if (c == '0') { ++i_; ok = true; return false; }
        if (c == '1') { ++i_; ok = true; return true; }
        ok = false;
        return false;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
};

}  // namespace

Path Path::from_svg_string(const std::string& d) {
    Path p;
    SvgScanner sc(d);

    Point2D cur{0.0f, 0.0f};        // current point
    Point2D subpath_start{0.0f, 0.0f};
    Point2D last_cubic_ctrl{0.0f, 0.0f};
    Point2D last_quad_ctrl{0.0f, 0.0f};
    bool had_cubic = false;       // was the previous command a C/c/S/s?
    bool had_quad = false;        // was the previous command a Q/q/T/t?
    char prev_cmd = '\0';

    while (!sc.at_end()) {
        char cmd = sc.peek();
        if (std::isalpha(static_cast<unsigned char>(cmd))) {
            sc.take_command();
        } else {
            // Implicit repeat of the previous command. After an M/m, the SVG
            // spec says the repeat is an L/l, not another move.
            if (prev_cmd == '\0') break;  // leading garbage — bail
            cmd = prev_cmd;
            if (cmd == 'M') cmd = 'L';
            else if (cmd == 'm') cmd = 'l';
        }

        const bool rel = std::islower(static_cast<unsigned char>(cmd)) != 0;
        const float ox = rel ? cur.x : 0.0f;
        const float oy = rel ? cur.y : 0.0f;
        bool ok = true;

        switch (std::toupper(static_cast<unsigned char>(cmd))) {
            case 'M': {
                const float x = sc.number(ok) + ox;
                const float y = sc.number(ok) + oy;
                if (!ok) return p;
                p.move_to(x, y);
                cur = {x, y};
                subpath_start = cur;
                had_cubic = had_quad = false;
                break;
            }
            case 'L': {
                const float x = sc.number(ok) + ox;
                const float y = sc.number(ok) + oy;
                if (!ok) return p;
                p.line_to(x, y);
                cur = {x, y};
                had_cubic = had_quad = false;
                break;
            }
            case 'H': {
                const float x = sc.number(ok) + ox;
                if (!ok) return p;
                p.line_to(x, cur.y);
                cur.x = x;
                had_cubic = had_quad = false;
                break;
            }
            case 'V': {
                const float y = sc.number(ok) + oy;
                if (!ok) return p;
                p.line_to(cur.x, y);
                cur.y = y;
                had_cubic = had_quad = false;
                break;
            }
            case 'C': {
                const float c1x = sc.number(ok) + ox;
                const float c1y = sc.number(ok) + oy;
                const float c2x = sc.number(ok) + ox;
                const float c2y = sc.number(ok) + oy;
                const float x = sc.number(ok) + ox;
                const float y = sc.number(ok) + oy;
                if (!ok) return p;
                p.cubic_to(c1x, c1y, c2x, c2y, x, y);
                last_cubic_ctrl = {c2x, c2y};
                cur = {x, y};
                had_cubic = true;
                had_quad = false;
                break;
            }
            case 'S': {
                // Smooth cubic: reflect the previous cubic's second control
                // point about the current point. With no previous cubic, the
                // reflection is the current point itself (SVG spec).
                const float c2x = sc.number(ok) + ox;
                const float c2y = sc.number(ok) + oy;
                const float x = sc.number(ok) + ox;
                const float y = sc.number(ok) + oy;
                if (!ok) return p;
                const Point2D c1 = had_cubic
                    ? Point2D{2.0f * cur.x - last_cubic_ctrl.x,
                            2.0f * cur.y - last_cubic_ctrl.y}
                    : cur;
                p.cubic_to(c1.x, c1.y, c2x, c2y, x, y);
                last_cubic_ctrl = {c2x, c2y};
                cur = {x, y};
                had_cubic = true;
                had_quad = false;
                break;
            }
            case 'Q': {
                const float cx = sc.number(ok) + ox;
                const float cy = sc.number(ok) + oy;
                const float x = sc.number(ok) + ox;
                const float y = sc.number(ok) + oy;
                if (!ok) return p;
                p.quad_to(cx, cy, x, y);
                last_quad_ctrl = {cx, cy};
                cur = {x, y};
                had_quad = true;
                had_cubic = false;
                break;
            }
            case 'T': {
                // Smooth quadratic: reflect the previous quad's control point.
                const float x = sc.number(ok) + ox;
                const float y = sc.number(ok) + oy;
                if (!ok) return p;
                const Point2D c = had_quad
                    ? Point2D{2.0f * cur.x - last_quad_ctrl.x,
                            2.0f * cur.y - last_quad_ctrl.y}
                    : cur;
                p.quad_to(c.x, c.y, x, y);
                last_quad_ctrl = c;
                cur = {x, y};
                had_quad = true;
                had_cubic = false;
                break;
            }
            case 'A': {
                const float rx = sc.number(ok);
                const float ry = sc.number(ok);
                const float rot_deg = sc.number(ok);
                const bool large_arc = sc.flag(ok);
                const bool sweep_flag = sc.flag(ok);
                const float x = sc.number(ok) + ox;
                const float y = sc.number(ok) + oy;
                if (!ok) return p;
                p.append_svg_arc(cur, rx, ry, rot_deg, large_arc, sweep_flag,
                                 Point2D{x, y});
                cur = {x, y};
                had_cubic = had_quad = false;
                break;
            }
            case 'Z': {
                p.close();
                cur = subpath_start;
                had_cubic = had_quad = false;
                break;
            }
            default:
                // Unknown command — stop and hand back the clean prefix.
                return p;
        }
        prev_cmd = cmd;
    }
    return p;
}

std::string Path::to_svg_string() const {
    if (is_empty()) return {};
    std::string out;
    out.reserve(verb_count() * 12);

    char buf[64];
    auto emit = [&](float v) {
        // %g gives the shortest round-trippable-enough form; SVG readers are
        // fine with either. Trailing spaces are trimmed by the caller below.
        std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
        out += buf;
    };

    for (Element el : *this) {
        switch (el.verb) {
            case Verb::move:
                out += 'M'; emit(el.points[0].x); out += ' '; emit(el.points[0].y);
                break;
            case Verb::line:
                out += 'L'; emit(el.points[0].x); out += ' '; emit(el.points[0].y);
                break;
            case Verb::quad:
                out += 'Q';
                emit(el.points[0].x); out += ' '; emit(el.points[0].y); out += ' ';
                emit(el.points[1].x); out += ' '; emit(el.points[1].y);
                break;
            case Verb::cubic:
                out += 'C';
                emit(el.points[0].x); out += ' '; emit(el.points[0].y); out += ' ';
                emit(el.points[1].x); out += ' '; emit(el.points[1].y); out += ' ';
                emit(el.points[2].x); out += ' '; emit(el.points[2].y);
                break;
            case Verb::close:
                out += 'Z';
                break;
        }
    }
    return out;
}

}  // namespace pulp::canvas
