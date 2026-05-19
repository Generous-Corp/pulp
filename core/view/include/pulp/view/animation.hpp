#pragma once

#include <cmath>
#include <functional>
#include <vector>
#include <algorithm>
#include <string>
#include <utility>

#include <pulp/view/motion.hpp>             // for motion::Provenance + publish_value
#include <pulp/view/motion_preferences.hpp>

#if defined(__has_include)
#  if __has_include(<source_location>)
#    include <source_location>
#  endif
#endif

namespace pulp::view {

class FrameClock; // forward declaration

// ── Easing functions ─────────────────────────────────────────────────────────

namespace easing {
    inline float linear(float t) { return t; }
    inline float ease_in_quad(float t) { return t * t; }
    inline float ease_out_quad(float t) { return t * (2.0f - t); }
    inline float ease_in_out_quad(float t) {
        return t < 0.5f ? 2.0f * t * t : -1.0f + (4.0f - 2.0f * t) * t;
    }
    inline float ease_in_cubic(float t) { return t * t * t; }
    inline float ease_out_cubic(float t) { float u = t - 1.0f; return u * u * u + 1.0f; }
    inline float ease_in_out_cubic(float t) {
        return t < 0.5f ? 4.0f * t * t * t : (t - 1.0f) * (2.0f * t - 2.0f) * (2.0f * t - 2.0f) + 1.0f;
    }
    inline float ease_in_expo(float t) { return t == 0 ? 0 : std::pow(2.0f, 10.0f * (t - 1.0f)); }
    inline float ease_out_expo(float t) { return t == 1.0f ? 1.0f : 1.0f - std::pow(2.0f, -10.0f * t); }
    inline float ease_out_elastic(float t) {
        if (t == 0 || t == 1) return t;
        return std::pow(2.0f, -10.0f * t) * std::sin((t - 0.075f) * (2.0f * 3.14159f) / 0.3f) + 1.0f;
    }
    inline float ease_out_bounce(float t) {
        if (t < 1.0f / 2.75f) return 7.5625f * t * t;
        if (t < 2.0f / 2.75f) { t -= 1.5f / 2.75f; return 7.5625f * t * t + 0.75f; }
        if (t < 2.5f / 2.75f) { t -= 2.25f / 2.75f; return 7.5625f * t * t + 0.9375f; }
        t -= 2.625f / 2.75f; return 7.5625f * t * t + 0.984375f;
    }
}

using EasingFunction = float(*)(float);

/// Compile-time animation limits for optimization.
/// Used to bound animation values without runtime checks.
struct StaticAnimationLimits {
    float min_value = 0.0f;
    float max_value = 1.0f;
    float min_duration = 0.0f;     // 0 = instant allowed
    float max_duration = 60.0f;    // 60 seconds max
    bool clamp_value = true;       // Whether to clamp output to [min, max]
};

// ── Tween ────────────────────────────────────────────────────────────────────

// Animates a float from start to end over a duration.
//
// Honors `MotionPreferences::current()` at construction and `reset()`:
//   - Full     → animate over the configured duration (default).
//   - Reduced  → scale the configured duration by `duration_scale`.
//   - Off      → finish immediately at `to`; no intermediate values
//                are produced.
class Tween {
public:
    Tween() = default;
    Tween(float from, float to, float duration_seconds, EasingFunction ease = easing::linear)
        : from_(from), to_(to),
          configured_duration_(duration_seconds),
          duration_(duration_seconds), ease_(ease) {
        apply_motion_policy_at_start();
    }

    // Advance by dt seconds. Returns current value.
    float advance(float dt) {
        elapsed_ += dt;
        if (duration_ <= 0.0f) {
            current_ = to_;
            return current_;
        }
        float t = std::clamp(elapsed_ / duration_, 0.0f, 1.0f);
        float eased = ease_(t);
        current_ = from_ + (to_ - from_) * eased;
        return current_;
    }

    float current() const { return current_; }
    bool finished() const { return elapsed_ >= duration_; }

    void reset() {
        elapsed_ = 0;
        current_ = from_;
        duration_ = configured_duration_;
        apply_motion_policy_at_start();
    }

    // ── Motion provenance (Phase 9) ─────────────────────────────────
    //
    // Attach a `motion::Provenance` envelope to this tween. When
    // `publish(view, metric)` is called, the publish channel stamps
    // the envelope onto every emitted event so an offline reader can
    // answer "what code created this tween?". Off by default — pre-
    // Phase-9 tweens that don't call this method behave identically.
    //
    // The convenience `PULP_MOTION_TWEEN(...)` macro auto-fills
    // `source_file` / `source_line` from `std::source_location` at the
    // construction site.
    void set_motion_provenance(std::string source_kind,
                               std::string source_id,
                               std::string source_file = {},
                               int source_line = 0) {
        provenance_.source_kind = std::move(source_kind);
        provenance_.source_id = std::move(source_id);
        provenance_.source_file = std::move(source_file);
        provenance_.source_line = source_line;
    }

    /// Set the full envelope. Useful when the provenance is built up
    /// somewhere else (e.g. a design-import codegen step).
    void set_motion_provenance(motion::Provenance p) {
        provenance_ = std::move(p);
    }

    const motion::Provenance& motion_provenance() const noexcept {
        return provenance_;
    }

    /// Publish the tween's current value through the motion publish
    /// channel under the given (view, metric) name. Stamps the tween's
    /// provenance envelope (set via `set_motion_provenance` or
    /// `PULP_MOTION_TWEEN`) onto the emitted event. Cheap no-op when
    /// motion tracing is off (`Coordinator::tracing_enabled() == false`).
    void publish(std::string view_name,
                 std::string metric_name,
                 motion::PublishOptions opts = {}) const {
        if (provenance_.is_set() && !opts.provenance.is_set()) {
            opts.provenance = provenance_;
        }
        motion::publish_value(std::move(view_name), std::move(metric_name),
                              static_cast<double>(current_), opts);
    }

private:
    /// Read `MotionPreferences::current()` once and apply it to `duration_`
    /// / `elapsed_` / `current_` so the tween starts already honoring the
    /// policy. `Off` snaps to the target on tick 0; `Reduced` shrinks the
    /// configured duration.
    void apply_motion_policy_at_start() {
        if (motion_policy_is_off()) {
            duration_ = 0.0f;
            elapsed_ = 0.0f;       // advance() returns to_ immediately
            current_ = to_;
            return;
        }
        if (MotionPreferences::current() == MotionPolicy::Reduced) {
            const double scale = MotionPreferences::current_duration_scale();
            duration_ = configured_duration_ * static_cast<float>(scale);
        }
    }

    float from_ = 0, to_ = 0;
    float configured_duration_ = 0;  ///< Author-supplied duration; reset()
                                     ///< re-applies the policy against this.
    float duration_ = 0;             ///< Effective duration after policy.
    float elapsed_ = 0;
    float current_ = 0;
    EasingFunction ease_ = easing::linear;
    motion::Provenance provenance_;  ///< Phase 9: opt-in attribution.
};

// ── PULP_MOTION_TWEEN (Phase 9) ─────────────────────────────────────────
//
// Convenience macro: constructs a `Tween` and stamps a `motion::Provenance`
// envelope with `source_kind = "tween"`, the supplied `source_id`, and
// `source_file` / `source_line` captured from `std::source_location` at
// the call site. The resulting tween is returned by value so the call
// site looks like `auto t = PULP_MOTION_TWEEN("hover-glow", 0.0f, 1.0f, 0.2f);`.
//
// `__FILE__` / `__LINE__` fall back if `<source_location>` isn't available
// (e.g. exotic toolchains); Pulp targets C++20 so the modern path is the
// default.
#ifdef __cpp_lib_source_location
#define PULP_MOTION_TWEEN(source_id, ...)                                       \
    ([](::pulp::view::Tween&& __pulp_tween_,                                    \
        const ::std::source_location __pulp_loc_ =                              \
            ::std::source_location::current())                                  \
            -> ::pulp::view::Tween {                                            \
        __pulp_tween_.set_motion_provenance(                                    \
            ::std::string{"tween"},                                             \
            ::std::string{(source_id)},                                         \
            ::std::string{__pulp_loc_.file_name()},                             \
            static_cast<int>(__pulp_loc_.line()));                              \
        return __pulp_tween_;                                                   \
    }(::pulp::view::Tween(__VA_ARGS__)))
#else
#define PULP_MOTION_TWEEN(source_id, ...)                                       \
    ([](::pulp::view::Tween&& __pulp_tween_) -> ::pulp::view::Tween {           \
        __pulp_tween_.set_motion_provenance(                                    \
            ::std::string{"tween"},                                             \
            ::std::string{(source_id)},                                         \
            ::std::string{__FILE__},                                            \
            static_cast<int>(__LINE__));                                        \
        return __pulp_tween_;                                                   \
    }(::pulp::view::Tween(__VA_ARGS__)))
#endif

// ── AnimationManager ─────────────────────────────────────────────────────────

// Manages active animations. Call tick() once per frame.
class AnimationManager {
public:
    struct AnimationId { int value = -1; };

    // Start a new animation with a callback
    AnimationId animate(float from, float to, float duration,
                       EasingFunction ease,
                       std::function<void(float)> on_update,
                       std::function<void()> on_complete = {}) {
        int id = next_id_++;
        animations_.push_back({id, {from, to, duration, ease},
                               std::move(on_update), std::move(on_complete)});
        return {id};
    }

    // Cancel an animation
    void cancel(AnimationId id) {
        animations_.erase(
            std::remove_if(animations_.begin(), animations_.end(),
                [id](const auto& a) { return a.id == id.value; }),
            animations_.end());
    }

    // Advance all animations by dt seconds
    void tick(float dt) {
        for (auto it = animations_.begin(); it != animations_.end();) {
            it->tween.advance(dt);
            if (it->on_update) it->on_update(it->tween.current());

            if (it->tween.finished()) {
                if (it->on_complete) it->on_complete();
                it = animations_.erase(it);
            } else {
                ++it;
            }
        }
    }

    int active_count() const { return static_cast<int>(animations_.size()); }
    bool has_active() const { return !animations_.empty(); }

private:
    struct Animation {
        int id;
        Tween tween;
        std::function<void(float)> on_update;
        std::function<void()> on_complete;
    };

    std::vector<Animation> animations_;
    int next_id_ = 0;
};

// ── easing_by_name ──────────────────────────────────────────────────────────

/// Resolve an easing function by name string. Returns linear for unknown names.
inline EasingFunction easing_by_name(const std::string& name) {
    if (name == "linear")             return easing::linear;
    if (name == "ease_in_quad")       return easing::ease_in_quad;
    if (name == "ease_out_quad")      return easing::ease_out_quad;
    if (name == "ease_in_out_quad")   return easing::ease_in_out_quad;
    if (name == "ease_in_cubic")      return easing::ease_in_cubic;
    if (name == "ease_out_cubic")     return easing::ease_out_cubic;
    if (name == "ease_in_out_cubic")  return easing::ease_in_out_cubic;
    if (name == "ease_in_expo")       return easing::ease_in_expo;
    if (name == "ease_out_expo")      return easing::ease_out_expo;
    if (name == "ease_out_elastic")   return easing::ease_out_elastic;
    if (name == "ease_out_bounce")    return easing::ease_out_bounce;
    return easing::linear;
}

// ── ValueAnimation ──────────────────────────────────────────────────────────

/// Lightweight, embeddable value animator for widget members.
/// No heap allocation. Designed to be a member variable.
class ValueAnimation {
public:
    ValueAnimation() = default;
    explicit ValueAnimation(float initial) : current_(initial), target_(initial), from_(initial) {}

    /// Set a new target. Starts animating from current value.
    ///
    /// Honors `MotionPreferences::current()` at start:
    ///   - Full     → animate over `duration`.
    ///   - Reduced  → scale `duration` by `duration_scale`.
    ///   - Off      → jump straight to `target`; no intermediate values.
    void animate_to(float target, float duration, EasingFunction ease = easing::ease_out_quad) {
        from_ = current_;
        target_ = target;
        configured_duration_ = duration;
        duration_ = duration;
        elapsed_ = 0;
        ease_ = ease;
        if (motion_policy_is_off()) {
            current_ = target;
            duration_ = 0.0f;
            animating_ = false;
            return;
        }
        if (MotionPreferences::current() == MotionPolicy::Reduced) {
            const double scale = MotionPreferences::current_duration_scale();
            duration_ = duration * static_cast<float>(scale);
        }
        if (duration_ <= 0) {
            current_ = target;
            animating_ = false;
        } else {
            animating_ = true;
        }
    }

    /// Snap to value immediately (no animation).
    void set(float value) {
        current_ = value;
        target_ = value;
        from_ = value;
        elapsed_ = 0;
        animating_ = false;
    }

    /// Advance by dt. Returns true if still animating.
    bool advance(float dt) {
        if (!animating_) return false;
        elapsed_ += dt;
        float t = std::clamp(elapsed_ / duration_, 0.0f, 1.0f);
        current_ = from_ + (target_ - from_) * ease_(t);
        if (t >= 1.0f) {
            current_ = target_;
            animating_ = false;
        }
        return animating_;
    }

    float value() const { return current_; }
    float target() const { return target_; }
    bool animating() const { return animating_; }

    /// Cancel in-flight animation, keep current value.
    void cancel() {
        target_ = current_;
        from_ = current_;
        animating_ = false;
    }

private:
    float current_ = 0;
    float target_ = 0;
    float from_ = 0;
    float configured_duration_ = 0;
    float duration_ = 0;
    float elapsed_ = 0;
    bool animating_ = false;
    EasingFunction ease_ = easing::ease_out_quad;
};

// ── Keyframe Animation (CSS @keyframes equivalent) ──────────────────────────

/// A single keyframe: offset (0-1) and value
struct Keyframe {
    float offset;  ///< 0.0 = start, 1.0 = end
    float value;
};

/// Multi-step keyframe animation with iteration, direction, fill mode
class KeyframeAnimation {
public:
    enum class Direction { normal, reverse, alternate };
    enum class FillMode { none, forwards, backwards, both };

    KeyframeAnimation() = default;

    void set_keyframes(std::vector<Keyframe> kf) {
        keyframes_ = std::move(kf);
        std::sort(keyframes_.begin(), keyframes_.end(),
            [](const Keyframe& a, const Keyframe& b) { return a.offset < b.offset; });
    }

    void set_duration(float seconds) { duration_ = seconds; }
    void set_iterations(float count) { iterations_ = count; }  // 0 = infinite
    void set_direction(Direction d) { direction_ = d; }
    void set_fill_mode(FillMode f) { fill_ = f; }
    void set_easing(EasingFunction e) { ease_ = e; }

    void start() { elapsed_ = 0; running_ = true; finished_ = false; completed_iterations_ = 0; }
    void stop() { running_ = false; }
    void pause() { running_ = false; }
    void resume() { running_ = true; }
    bool is_running() const { return running_; }
    bool is_finished() const { return finished_; }

    /// Advance by dt seconds, return current interpolated value
    float advance(float dt) {
        if (!running_ || keyframes_.size() < 2 || duration_ <= 0) return value_;

        elapsed_ += dt;
        float iteration_progress = elapsed_ / duration_;

        if (iterations_ > 0 && iteration_progress >= iterations_) {
            finished_ = true;
            running_ = false;
            iteration_progress = iterations_;
        }

        // Current iteration number and progress within it. When a finite
        // animation finishes exactly on an iteration boundary, keep the
        // terminal frame at t=1 for the final iteration instead of wrapping
        // to t=0 of the next iteration.
        float iter_num = std::floor(iteration_progress);
        float t = iteration_progress - iter_num;
        if (finished_ && iterations_ > 0.0f && t == 0.0f && iteration_progress > 0.0f) {
            iter_num -= 1.0f;
            t = 1.0f;
        }
        completed_iterations_ = static_cast<int>(iter_num);

        // Direction
        bool reversed = false;
        if (direction_ == Direction::reverse) reversed = true;
        else if (direction_ == Direction::alternate) reversed = (static_cast<int>(iter_num) % 2) == 1;

        if (reversed) t = 1.0f - t;

        // Apply easing
        t = ease_(std::clamp(t, 0.0f, 1.0f));

        // Interpolate between keyframes
        value_ = interpolate(t);

        // Fill mode
        if (finished_) {
            if (fill_ == FillMode::forwards || fill_ == FillMode::both)
                value_ = interpolate(reversed ? 0.0f : 1.0f);
        }

        return value_;
    }

    float value() const { return value_; }

private:
    float interpolate(float t) const {
        if (keyframes_.empty()) return 0;
        if (t <= keyframes_.front().offset) return keyframes_.front().value;
        if (t >= keyframes_.back().offset) return keyframes_.back().value;

        for (size_t i = 1; i < keyframes_.size(); ++i) {
            if (t <= keyframes_[i].offset) {
                float local_t = (t - keyframes_[i-1].offset) /
                               (keyframes_[i].offset - keyframes_[i-1].offset);
                return keyframes_[i-1].value + local_t * (keyframes_[i].value - keyframes_[i-1].value);
            }
        }
        return keyframes_.back().value;
    }

    std::vector<Keyframe> keyframes_;
    float duration_ = 1.0f;
    float iterations_ = 1.0f;  ///< 0 = infinite
    float elapsed_ = 0;
    float value_ = 0;
    int completed_iterations_ = 0;
    bool running_ = false;
    bool finished_ = false;
    Direction direction_ = Direction::normal;
    FillMode fill_ = FillMode::none;
    EasingFunction ease_ = easing::linear;
};

} // namespace pulp::view
