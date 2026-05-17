#pragma once

// AnimatorSetBuilder — compose animations in sequence or parallel groups.

#include <algorithm>
#include <pulp/view/animation.hpp>
#include <pulp/view/motion.hpp>
#include <vector>
#include <memory>
#include <functional>
#include <string>
#include <utility>

namespace pulp::view {

/// A step in an animation sequence — either a tween or a parallel group
class AnimationStep {
public:
    virtual ~AnimationStep() = default;
    virtual float advance(float dt) = 0;
    virtual bool finished() const = 0;
    virtual void reset() = 0;
};

/// Single tween step
class TweenStep : public AnimationStep {
public:
    TweenStep(Tween tween, std::function<void(float)> apply)
        : tween_(tween), apply_(std::move(apply)) {}

    float advance(float dt) override {
        float v = tween_.advance(dt);
        if (apply_) apply_(v);
        return v;
    }
    bool finished() const override { return tween_.finished(); }
    void reset() override { tween_.reset(); }

private:
    Tween tween_;
    std::function<void(float)> apply_;
};

/// Parallel group — all steps run simultaneously
class ParallelStep : public AnimationStep {
public:
    void add(std::unique_ptr<AnimationStep> step) {
        steps_.push_back(std::move(step));
    }

    float advance(float dt) override {
        for (auto& s : steps_)
            if (!s->finished())
                s->advance(dt);
        return 0;
    }

    bool finished() const override {
        for (auto& s : steps_)
            if (!s->finished()) return false;
        return true;
    }

    void reset() override {
        for (auto& s : steps_)
            s->reset();
    }

private:
    std::vector<std::unique_ptr<AnimationStep>> steps_;
};

/// Builder for composing animation sequences and parallel groups
class AnimatorSetBuilder {
public:
    /// Phase 9: assign a logical name to this animator set. Threaded
    /// through to the `Runner`, which stamps it as
    /// `source_kind="animator-set", source_id=<name>` on every emitted
    /// publish event. Off by default — pre-Phase-9 builders that never
    /// call `name()` produce events with no provenance.
    AnimatorSetBuilder& name(std::string n) {
        name_ = std::move(n);
        return *this;
    }

    const std::string& configured_name() const noexcept { return name_; }

    /// Add a tween that runs in sequence
    AnimatorSetBuilder& then(float from, float to, float duration,
                             std::function<void(float)> apply,
                             EasingFunction ease = easing::linear) {
        steps_.push_back(std::make_unique<TweenStep>(
            Tween(from, to, duration, ease), std::move(apply)));
        return *this;
    }

    /// Add a delay (tween from 0 to 0)
    AnimatorSetBuilder& delay(float seconds) {
        steps_.push_back(std::make_unique<TweenStep>(
            Tween(0, 0, seconds), nullptr));
        return *this;
    }

    /// Start a parallel group — all tweens added until end_parallel() run together
    AnimatorSetBuilder& begin_parallel() {
        parallel_ = std::make_unique<ParallelStep>();
        return *this;
    }

    /// Add a tween to the current parallel group
    AnimatorSetBuilder& with(float from, float to, float duration,
                             std::function<void(float)> apply,
                             EasingFunction ease = easing::linear) {
        if (parallel_) {
            parallel_->add(std::make_unique<TweenStep>(
                Tween(from, to, duration, ease), std::move(apply)));
        }
        return *this;
    }

    /// End parallel group and add it to the sequence
    AnimatorSetBuilder& end_parallel() {
        if (parallel_) {
            steps_.push_back(std::move(parallel_));
        }
        return *this;
    }

    /// Build and return the step sequence
    std::vector<std::unique_ptr<AnimationStep>> build() {
        return std::move(steps_);
    }

    /// Run the built animation. Advances through sequence, returns true when all done.
    class Runner {
    public:
        explicit Runner(std::vector<std::unique_ptr<AnimationStep>> steps,
                        std::string name = {})
            : steps_(std::move(steps)) {
            if (!name.empty()) {
                provenance_.source_kind = "animator-set";
                provenance_.source_id = std::move(name);
            }
        }

        bool advance(float dt) {
            while (current_ < static_cast<int>(steps_.size()) && dt > 0) {
                steps_[current_]->advance(dt);
                if (steps_[current_]->finished()) {
                    ++current_;
                    // Carry leftover time to the next step
                } else {
                    break;
                }
            }
            return current_ >= static_cast<int>(steps_.size());
        }

        bool finished() const {
            return current_ >= static_cast<int>(steps_.size());
        }

        void reset() {
            current_ = 0;
            for (auto& s : steps_)
                s->reset();
        }

        /// Phase 9: publish the runner's current scalar through the motion
        /// publish channel, stamping the animator-set name (configured
        /// via `AnimatorSetBuilder::name()`) as provenance.
        void publish(std::string view_name,
                     std::string metric_name,
                     double value,
                     motion::PublishOptions opts = {}) const {
            if (provenance_.is_set() && !opts.provenance.is_set()) {
                opts.provenance = provenance_;
            }
            motion::publish_value(std::move(view_name), std::move(metric_name),
                                  value, opts);
        }

        const motion::Provenance& motion_provenance() const noexcept {
            return provenance_;
        }

    private:
        std::vector<std::unique_ptr<AnimationStep>> steps_;
        int current_ = 0;
        motion::Provenance provenance_;
    };

    Runner build_runner() {
        return Runner(std::move(steps_), name_);
    }

private:
    std::vector<std::unique_ptr<AnimationStep>> steps_;
    std::unique_ptr<ParallelStep> parallel_;
    std::string name_;  ///< Phase 9: motion-provenance source_id.
};

// ── Additional easing functions ─────────────────────────────────────────

namespace easing {
    /// Cubic bezier easing (simplified — approximates CSS cubic-bezier)
    inline float cubic_bezier(float t, float x1, float y1, float x2, float y2) {
        // Simple Newton-Raphson approximation
        float cx = 3.0f * x1;
        float bx = 3.0f * (x2 - x1) - cx;
        float ax = 1.0f - cx - bx;

        float cy = 3.0f * y1;
        float by = 3.0f * (y2 - y1) - cy;
        float ay = 1.0f - cy - by;

        // Solve for t given x using Newton's method
        float guess = t;
        for (int i = 0; i < 8; ++i) {
            float x = ((ax * guess + bx) * guess + cx) * guess - t;
            float dx = (3.0f * ax * guess + 2.0f * bx) * guess + cx;
            if (std::abs(dx) < 1e-7f) break;
            guess -= x / dx;
        }

        return ((ay * guess + by) * guess + cy) * guess;
    }

    /// Spring physics easing (damped oscillation)
    inline float spring(float t, float stiffness = 100.0f, float damping = 10.0f) {
        float omega = std::sqrt(stiffness);
        float zeta = damping / (2.0f * omega);

        if (zeta < 1.0f) {
            // Underdamped — oscillates
            float omega_d = omega * std::sqrt(1.0f - zeta * zeta);
            return 1.0f - std::exp(-zeta * omega * t) *
                   (std::cos(omega_d * t) + (zeta * omega / omega_d) * std::sin(omega_d * t));
        }

        // Critically damped or overdamped
        return 1.0f - (1.0f + omega * t) * std::exp(-omega * t);
    }

    /// Ease-in-out back (slight overshoot)
    inline float ease_in_out_back(float t) {
        float s = 1.70158f * 1.525f;
        if (t < 0.5f) {
            return (2.0f * t * t * ((s + 1.0f) * 2.0f * t - s)) * 0.5f;
        }
        float u = 2.0f * t - 2.0f;
        return (u * u * ((s + 1.0f) * u + s) + 2.0f) * 0.5f;
    }
}

// ── 3D Math (Quaternion, Vector3D) ──────────────────────────────────────

struct Vector3D {
    float x = 0, y = 0, z = 0;

    Vector3D operator+(const Vector3D& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vector3D operator-(const Vector3D& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vector3D operator*(float s) const { return {x * s, y * s, z * s}; }

    float dot(const Vector3D& o) const { return x * o.x + y * o.y + z * o.z; }
    Vector3D cross(const Vector3D& o) const {
        return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x};
    }
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    Vector3D normalized() const {
        float len = length();
        return len > 0 ? Vector3D{x / len, y / len, z / len} : Vector3D{};
    }
};

struct Quaternion {
    float w = 1, x = 0, y = 0, z = 0;

    static Quaternion identity() { return {1, 0, 0, 0}; }

    static Quaternion from_axis_angle(const Vector3D& axis, float radians) {
        float half = radians * 0.5f;
        float s = std::sin(half);
        auto n = axis.normalized();
        return {std::cos(half), n.x * s, n.y * s, n.z * s};
    }

    Quaternion operator*(const Quaternion& q) const {
        return {
            w * q.w - x * q.x - y * q.y - z * q.z,
            w * q.x + x * q.w + y * q.z - z * q.y,
            w * q.y - x * q.z + y * q.w + z * q.x,
            w * q.z + x * q.y - y * q.x + z * q.w
        };
    }

    Quaternion conjugate() const { return {w, -x, -y, -z}; }

    float length() const { return std::sqrt(w * w + x * x + y * y + z * z); }

    Quaternion normalized() const {
        float len = length();
        return len > 0 ? Quaternion{w / len, x / len, y / len, z / len} : identity();
    }

    /// Spherical linear interpolation
    static Quaternion slerp(const Quaternion& a, const Quaternion& b, float t) {
        float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        Quaternion b2 = dot < 0 ? Quaternion{-b.w, -b.x, -b.y, -b.z} : b;
        dot = std::abs(dot);

        if (dot > 0.9995f) {
            // Linear interpolation for very close quaternions
            return Quaternion{
                a.w + t * (b2.w - a.w),
                a.x + t * (b2.x - a.x),
                a.y + t * (b2.y - a.y),
                a.z + t * (b2.z - a.z)
            }.normalized();
        }

        float theta = std::acos(dot);
        float sin_theta = std::sin(theta);
        float wa = std::sin((1.0f - t) * theta) / sin_theta;
        float wb = std::sin(t * theta) / sin_theta;

        return Quaternion{
            wa * a.w + wb * b2.w,
            wa * a.x + wb * b2.x,
            wa * a.y + wb * b2.y,
            wa * a.z + wb * b2.z
        };
    }

    /// Rotate a vector by this quaternion
    Vector3D rotate(const Vector3D& v) const {
        Quaternion p{0, v.x, v.y, v.z};
        Quaternion result = (*this) * p * conjugate();
        return {result.x, result.y, result.z};
    }
};

/// Draggable 3D orientation — arcball rotation from mouse drag
class Draggable3DOrientation {
public:
    /// Begin a drag at the given 2D screen position (normalized -1 to 1)
    void begin_drag(float x, float y) {
        dragging_ = true;
        drag_start_ = project_to_sphere(x, y);
        start_orientation_ = orientation_;
    }

    /// Update during drag
    void update_drag(float x, float y) {
        if (!dragging_) return;

        Vector3D current = project_to_sphere(x, y);
        Vector3D axis = drag_start_.cross(current);
        float dot = drag_start_.dot(current);

        if (axis.length() > 1e-6f) {
            float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
            Quaternion rotation = Quaternion::from_axis_angle(axis.normalized(), angle);
            orientation_ = rotation * start_orientation_;
        }
    }

    /// End the drag
    void end_drag() { dragging_ = false; }

    /// Current orientation quaternion
    const Quaternion& orientation() const { return orientation_; }

    /// Reset to identity (also cancels any active drag)
    void reset() {
        orientation_ = Quaternion::identity();
        dragging_ = false;
        drag_start_ = {};
        start_orientation_ = Quaternion::identity();
    }

private:
    Quaternion orientation_ = Quaternion::identity();
    Quaternion start_orientation_;
    Vector3D drag_start_;
    bool dragging_ = false;

    static Vector3D project_to_sphere(float x, float y) {
        float d = x * x + y * y;
        if (d < 1.0f)
            return {x, y, std::sqrt(1.0f - d)};
        float scale = 1.0f / std::sqrt(d);
        return {x * scale, y * scale, 0};
    }
};

}  // namespace pulp::view
