#include <pulp/canvas/path_measure.hpp>

#include <algorithm>
#include <cmath>

namespace pulp::canvas {
namespace {

constexpr float kMinTolerance = 1.0f / 1024.0f;
constexpr int kMaxSubdivisions = 1024;

float distance_between(Point2D a, Point2D b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
}

Point2D lerp(Point2D a, Point2D b, float t) {
    return Point2D{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t};
}

Point2D eval_quad(const Point2D p[3], float t) {
    const Point2D ab = lerp(p[0], p[1], t);
    const Point2D bc = lerp(p[1], p[2], t);
    return lerp(ab, bc, t);
}

Point2D eval_cubic(const Point2D p[4], float t) {
    const Point2D ab = lerp(p[0], p[1], t);
    const Point2D bc = lerp(p[1], p[2], t);
    const Point2D cd = lerp(p[2], p[3], t);
    return lerp(lerp(ab, bc, t), lerp(bc, cd, t), t);
}

/// Subdivision count that holds the chord error under `tolerance`.
///
/// Both bounds come from the standard second-difference estimate: the error of
/// an n-segment polyline falls as 1/n^2 in the magnitude of the curve's second
/// difference, which is zero for a straight control polygon. A curve whose
/// control points are collinear therefore costs one segment, not a fixed
/// minimum.
int quad_subdivisions(const Point2D p[3], float tolerance) {
    const float dx = p[0].x - 2.0f * p[1].x + p[2].x;
    const float dy = p[0].y - 2.0f * p[1].y + p[2].y;
    const float d = std::sqrt(dx * dx + dy * dy);
    if (d <= 0.0f)
        return 1;
    const int n = static_cast<int>(std::ceil(std::sqrt(d / (8.0f * tolerance))));
    return std::clamp(n, 1, kMaxSubdivisions);
}

int cubic_subdivisions(const Point2D p[4], float tolerance) {
    const float d1x = p[0].x - 2.0f * p[1].x + p[2].x;
    const float d1y = p[0].y - 2.0f * p[1].y + p[2].y;
    const float d2x = p[1].x - 2.0f * p[2].x + p[3].x;
    const float d2y = p[1].y - 2.0f * p[2].y + p[3].y;
    const float d = std::sqrt(std::max(d1x * d1x + d1y * d1y, d2x * d2x + d2y * d2y));
    if (d <= 0.0f)
        return 1;
    const int n = static_cast<int>(std::ceil(std::sqrt(0.75f * d / tolerance)));
    return std::clamp(n, 1, kMaxSubdivisions);
}

/// de Casteljau split of a quad at `t`; `left` and `right` may be null.
void split_quad(const Point2D p[3], float t, Point2D* left, Point2D* right) {
    const Point2D ab = lerp(p[0], p[1], t);
    const Point2D bc = lerp(p[1], p[2], t);
    const Point2D mid = lerp(ab, bc, t);
    if (left) {
        left[0] = p[0];
        left[1] = ab;
        left[2] = mid;
    }
    if (right) {
        right[0] = mid;
        right[1] = bc;
        right[2] = p[2];
    }
}

void split_cubic(const Point2D p[4], float t, Point2D* left, Point2D* right) {
    const Point2D ab = lerp(p[0], p[1], t);
    const Point2D bc = lerp(p[1], p[2], t);
    const Point2D cd = lerp(p[2], p[3], t);
    const Point2D abc = lerp(ab, bc, t);
    const Point2D bcd = lerp(bc, cd, t);
    const Point2D mid = lerp(abc, bcd, t);
    if (left) {
        left[0] = p[0];
        left[1] = ab;
        left[2] = abc;
        left[3] = mid;
    }
    if (right) {
        right[0] = mid;
        right[1] = bcd;
        right[2] = cd;
        right[3] = p[3];
    }
}

} // namespace

PathMeasure::PathMeasure(const Path& path, float tolerance)
    : tolerance_(std::max(tolerance, kMinTolerance)) {
    Point2D cursor{};
    Point2D contour_start{};
    bool have_contour = false;
    size_t contour_first_segment = 0;
    size_t contour_first_sample = 0;
    float contour_start_distance = 0.0f;

    // A contour only becomes real once it has length, so `move_to` alone —
    // and a `close` on a contour that never moved — contribute nothing.
    auto begin_contour = [&](Point2D at) {
        contour_first_segment = segments_.size();
        contour_first_sample = samples_.size();
        contour_start_distance = total_length_;
        contour_start = at;
        have_contour = true;
        samples_.push_back(
            Sample{total_length_, at, static_cast<uint32_t>(segments_.size()), 0.0f});
    };

    auto finish_contour = [&](bool closed) {
        if (!have_contour)
            return;
        const float length = total_length_ - contour_start_distance;
        if (length > 0.0f) {
            contours_.push_back(Contour{
                contour_first_sample, samples_.size() - contour_first_sample, contour_first_segment,
                segments_.size() - contour_first_segment, contour_start_distance, length, closed});
        } else {
            // Zero-length contour: drop its bookkeeping so every reported
            // contour has positive length.
            samples_.resize(contour_first_sample);
            segments_.resize(contour_first_segment);
        }
        have_contour = false;
    };

    // Emit one segment plus its flattened samples. `count` is 2, 3 or 4.
    auto add_segment = [&](const Point2D* pts, uint8_t count) {
        if (!have_contour)
            begin_contour(pts[0]);

        Segment seg;
        seg.count = count;
        for (uint8_t i = 0; i < count; ++i)
            seg.points[i] = pts[i];
        seg.contour_index = static_cast<uint32_t>(contours_.size());
        const auto segment_index = static_cast<uint32_t>(segments_.size());
        segments_.push_back(seg);

        const int n = count == 2   ? 1
                      : count == 3 ? quad_subdivisions(pts, tolerance_)
                                   : cubic_subdivisions(pts, tolerance_);
        Point2D previous = pts[0];
        for (int i = 1; i <= n; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(n);
            const Point2D point = count == 2   ? lerp(pts[0], pts[1], t)
                                  : count == 3 ? eval_quad(pts, t)
                                               : eval_cubic(pts, t);
            total_length_ += distance_between(previous, point);
            samples_.push_back(Sample{total_length_, point, segment_index, t});
            previous = point;
        }
        cursor = pts[count - 1];
    };

    for (Path::Element el : path) {
        switch (el.verb) {
        case Path::Verb::move:
            finish_contour(false);
            cursor = el.points[0];
            contour_start = cursor;
            break;
        case Path::Verb::line: {
            const Point2D pts[2] = {cursor, el.points[0]};
            add_segment(pts, 2);
            break;
        }
        case Path::Verb::quad: {
            const Point2D pts[3] = {cursor, el.points[0], el.points[1]};
            add_segment(pts, 3);
            break;
        }
        case Path::Verb::cubic: {
            const Point2D pts[4] = {cursor, el.points[0], el.points[1], el.points[2]};
            add_segment(pts, 4);
            break;
        }
        case Path::Verb::close: {
            if (have_contour) {
                const Point2D pts[2] = {cursor, contour_start};
                if (distance_between(pts[0], pts[1]) > 0.0f)
                    add_segment(pts, 2);
                finish_contour(true);
            }
            cursor = contour_start;
            break;
        }
        }
    }
    finish_contour(false);
}

float PathMeasure::contour_length(size_t index) const {
    return index < contours_.size() ? contours_[index].length : 0.0f;
}

bool PathMeasure::contour_is_closed(size_t index) const {
    return index < contours_.size() && contours_[index].closed;
}

float PathMeasure::contour_start(size_t index) const {
    return index < contours_.size() ? contours_[index].start_distance : 0.0f;
}

size_t PathMeasure::sample_index_for(float distance) const {
    // Last sample whose distance is <= `distance`.
    const auto it =
        std::upper_bound(samples_.begin(), samples_.end(), distance,
                         [](float value, const Sample& s) { return value < s.distance; });
    if (it == samples_.begin())
        return 0;
    return static_cast<size_t>(std::distance(samples_.begin(), it) - 1);
}

std::optional<PathPose> PathMeasure::pose_at(float distance) const {
    if (samples_.empty() || total_length_ <= 0.0f)
        return std::nullopt;

    const float clamped = std::clamp(distance, 0.0f, total_length_);
    const size_t index = sample_index_for(clamped);
    if (index + 1 >= samples_.size()) {
        // At or past the final sample: report the last point, with the
        // tangent of the segment that arrives there.
        const Sample& last = samples_.back();
        PathPose pose;
        pose.position = last.position;
        if (samples_.size() >= 2) {
            const Point2D previous = samples_[samples_.size() - 2].position;
            const float d = distance_between(previous, last.position);
            if (d > 0.0f) {
                pose.tangent =
                    Point2D{(last.position.x - previous.x) / d, (last.position.y - previous.y) / d};
            }
        }
        return pose;
    }

    const Sample& a = samples_[index];
    const Sample& b = samples_[index + 1];
    const float span = b.distance - a.distance;
    const float fraction = span > 0.0f ? (clamped - a.distance) / span : 0.0f;

    PathPose pose;
    pose.position = lerp(a.position, b.position, fraction);
    const float d = distance_between(a.position, b.position);
    if (d > 0.0f) {
        pose.tangent =
            Point2D{(b.position.x - a.position.x) / d, (b.position.y - a.position.y) / d};
    }
    return pose;
}

std::optional<Point2D> PathMeasure::position_at(float distance) const {
    if (const auto pose = pose_at(distance))
        return pose->position;
    return std::nullopt;
}

std::optional<Point2D> PathMeasure::tangent_at(float distance) const {
    if (const auto pose = pose_at(distance))
        return pose->tangent;
    return std::nullopt;
}

Path PathMeasure::segment(float start, float end) const {
    Path out;
    if (samples_.empty() || total_length_ <= 0.0f)
        return out;

    const float a = std::clamp(start, 0.0f, total_length_);
    const float b = std::clamp(end, 0.0f, total_length_);
    if (a >= b)
        return out;

    // Map a distance to (segment index, parameter on that segment). Between
    // two samples of the SAME segment the parameter interpolates; when the
    // bracketing samples straddle a segment join, the run starts at the new
    // segment's t = 0.
    auto locate = [&](float distance) -> std::pair<uint32_t, float> {
        const size_t index = sample_index_for(distance);
        if (index + 1 >= samples_.size()) {
            const Sample& last = samples_.back();
            return {last.segment_index, last.t};
        }
        const Sample& lo = samples_[index];
        const Sample& hi = samples_[index + 1];
        const float span = hi.distance - lo.distance;
        const float fraction = span > 0.0f ? (distance - lo.distance) / span : 0.0f;
        const float lo_t = lo.segment_index == hi.segment_index ? lo.t : 0.0f;
        return {hi.segment_index, lo_t + (hi.t - lo_t) * fraction};
    };

    const auto [first_segment, first_t] = locate(a);
    const auto [last_segment, last_t] = locate(b);

    // Extract the part of `seg` between parameters t0 and t1 (0 <= t0 < t1 <= 1).
    auto subcurve = [](const Segment& seg, float t0, float t1) {
        Segment out_seg = seg;
        if (seg.count == 2) {
            const Point2D p0 = lerp(seg.points[0], seg.points[1], t0);
            const Point2D p1 = lerp(seg.points[0], seg.points[1], t1);
            out_seg.points[0] = p0;
            out_seg.points[1] = p1;
            return out_seg;
        }
        // Trim the head, then the tail, remapping t1 into the trimmed curve.
        Point2D right[4]{};
        if (seg.count == 3) {
            split_quad(seg.points, t0, nullptr, right);
        } else {
            split_cubic(seg.points, t0, nullptr, right);
        }
        const float remaining = 1.0f - t0;
        const float t = remaining > 0.0f ? std::clamp((t1 - t0) / remaining, 0.0f, 1.0f) : 0.0f;
        Point2D left[4]{};
        if (seg.count == 3) {
            split_quad(right, t, left, nullptr);
        } else {
            split_cubic(right, t, left, nullptr);
        }
        for (uint8_t i = 0; i < seg.count; ++i)
            out_seg.points[i] = left[i];
        return out_seg;
    };

    auto emit = [&out](const Segment& seg, bool first) {
        if (first)
            out.move_to(seg.points[0].x, seg.points[0].y);
        switch (seg.count) {
        case 2:
            out.line_to(seg.points[1].x, seg.points[1].y);
            break;
        case 3:
            out.quad_to(seg.points[1].x, seg.points[1].y, seg.points[2].x, seg.points[2].y);
            break;
        default:
            out.cubic_to(seg.points[1].x, seg.points[1].y, seg.points[2].x, seg.points[2].y,
                         seg.points[3].x, seg.points[3].y);
            break;
        }
    };

    if (first_segment == last_segment) {
        emit(subcurve(segments_[first_segment], first_t, last_t), true);
        return out;
    }

    emit(subcurve(segments_[first_segment], first_t, 1.0f), true);
    for (uint32_t i = first_segment + 1; i < last_segment; ++i)
        emit(segments_[i], false);
    if (last_t > 0.0f)
        emit(subcurve(segments_[last_segment], 0.0f, last_t), false);
    return out;
}

} // namespace pulp::canvas
