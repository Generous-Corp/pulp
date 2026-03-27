#pragma once

#include <cmath>
#include <functional>
#include <vector>
#include <algorithm>
#include <string>

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

// ── Tween ────────────────────────────────────────────────────────────────────

// Animates a float from start to end over a duration
class Tween {
public:
    Tween() = default;
    Tween(float from, float to, float duration_seconds, EasingFunction ease = easing::linear)
        : from_(from), to_(to), duration_(duration_seconds), ease_(ease) {}

    // Advance by dt seconds. Returns current value.
    float advance(float dt) {
        elapsed_ += dt;
        float t = std::clamp(elapsed_ / duration_, 0.0f, 1.0f);
        float eased = ease_(t);
        current_ = from_ + (to_ - from_) * eased;
        return current_;
    }

    float current() const { return current_; }
    bool finished() const { return elapsed_ >= duration_; }

    void reset() { elapsed_ = 0; current_ = from_; }

private:
    float from_ = 0, to_ = 0;
    float duration_ = 0;
    float elapsed_ = 0;
    float current_ = 0;
    EasingFunction ease_ = easing::linear;
};

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
    void animate_to(float target, float duration, EasingFunction ease = easing::ease_out_quad) {
        from_ = current_;
        target_ = target;
        duration_ = duration;
        elapsed_ = 0;
        ease_ = ease;
        if (duration <= 0) {
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
    float duration_ = 0;
    float elapsed_ = 0;
    bool animating_ = false;
    EasingFunction ease_ = easing::ease_out_quad;
};

} // namespace pulp::view
