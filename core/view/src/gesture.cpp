#include <pulp/view/gesture.hpp>
#include <pulp/view/view.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>

namespace pulp::view {
namespace {

double default_timestamp_seconds() {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<double>(clock::now().time_since_epoch()).count();
}

float distance_between(Point a, Point b) {
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    return std::sqrt(dx * dx + dy * dy);
}

Point subtract(Point a, Point b) {
    return {a.x - b.x, a.y - b.y};
}

float magnitude(Point p) {
    return std::sqrt(p.x * p.x + p.y * p.y);
}

Point midpoint(Point a, Point b) {
    return {(a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f};
}

float angle_between(Point a, Point b) {
    return std::atan2(b.y - a.y, b.x - a.x);
}

float normalize_angle_delta(float radians) {
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = kPi * 2.0f;
    while (radians > kPi) radians -= kTwoPi;
    while (radians < -kPi) radians += kTwoPi;
    return radians;
}

bool contains_recognizer(const std::vector<GestureRecognizer*>& list,
                         const GestureRecognizer* recognizer) {
    return std::find(list.begin(), list.end(), recognizer) != list.end();
}

bool is_terminal(GestureState state) {
    return state == GestureState::ended ||
           state == GestureState::cancelled ||
           state == GestureState::failed;
}

bool is_recognized(GestureState state) {
    return state == GestureState::began ||
           state == GestureState::changed ||
           state == GestureState::ended;
}

bool is_release_or_cancel(const MouseEvent& event) {
    if (event.is_cancelled) return true;
    if (event.phase == MousePhase::release) return true;
    return event.phase == MousePhase::automatic && !event.is_down && !event.is_wheel;
}

bool is_press_without_session(const MouseEvent& event, bool has_session) {
    if (has_session) return false;
    return event.phase == MousePhase::automatic ? event.is_down
                                                : event.phase == MousePhase::press;
}

Point root_position_for(const MouseEvent& event) {
    if (event.window_position.x != 0.0f || event.window_position.y != 0.0f)
        return event.window_position;
    return event.position;
}

Point root_to_local(Point root_position, View* view, View& root) {
    Point local = root_position;
    for (View* v = view; v && v != &root; v = v->parent()) {
        local.x -= v->bounds().x;
        local.y -= v->bounds().y;
    }
    return local;
}

bool is_ancestor_or_self(const View* ancestor, const View* view) {
    for (const View* current = view; current; current = current->parent()) {
        if (current == ancestor) return true;
    }
    return false;
}

bool views_share_gesture_branch(const View* first, const View* second) {
    if (!first || !second) return true;
    return is_ancestor_or_self(first, second) ||
           is_ancestor_or_self(second, first);
}

}  // namespace

void GestureRecognizer::require_to_fail(GestureRecognizer& other) {
    if (&other == this || contains_recognizer(require_failures_, &other)) return;
    require_failures_.push_back(&other);
}

void GestureRecognizer::allow_simultaneous_with(GestureRecognizer& other) {
    if (&other == this) return;
    if (!contains_recognizer(simultaneous_, &other))
        simultaneous_.push_back(&other);
    if (!contains_recognizer(other.simultaneous_, this))
        other.simultaneous_.push_back(this);
}

bool GestureRecognizer::requires_failure_of(const GestureRecognizer& other) const {
    return contains_recognizer(require_failures_, &other);
}

bool GestureRecognizer::can_recognize_simultaneously_with(
        const GestureRecognizer& other) const {
    return contains_recognizer(simultaneous_, &other) ||
           contains_recognizer(other.simultaneous_, this);
}

void GestureRecognizer::transition_to(GestureState state) {
    if (state_ == GestureState::failed || state_ == GestureState::cancelled)
        return;
    state_ = state;
    if (state == GestureState::began ||
        state == GestureState::changed ||
        state == GestureState::ended ||
        state == GestureState::cancelled) {
        pending_callbacks_.push_back(state);
    }
}

void GestureRecognizer::reset_to_possible() {
    state_ = GestureState::possible;
    pending_callbacks_.clear();
    on_reset();
}

void GestureRecognizer::fail() {
    if (is_terminal(state_)) return;
    state_ = GestureState::failed;
    pending_callbacks_.clear();
}

void GestureRecognizer::remove_relationships_to(const GestureRecognizer& other) {
    auto remove = [&other](std::vector<GestureRecognizer*>& recognizers) {
        recognizers.erase(
            std::remove(recognizers.begin(), recognizers.end(), &other),
            recognizers.end());
    };
    remove(require_failures_);
    remove(simultaneous_);
}

void GestureRecognizer::cancel() {
    if (state_ == GestureState::began || state_ == GestureState::changed) {
        state_ = GestureState::cancelled;
        pending_callbacks_.push_back(GestureState::cancelled);
        return;
    }
    fail();
}

void GestureRecognizer::dispatch_pending_callbacks() {
    auto pending = std::move(pending_callbacks_);
    pending_callbacks_.clear();
    for (GestureState state : pending) {
        switch (state) {
            case GestureState::began:
                if (on_began) on_began(*this);
                break;
            case GestureState::changed:
                if (on_changed) on_changed(*this);
                break;
            case GestureState::ended:
                if (on_ended) on_ended(*this);
                break;
            case GestureState::cancelled:
                if (on_cancelled) on_cancelled(*this);
                break;
            default:
                break;
        }
    }
}

TapRecognizer::TapRecognizer(int required_tap_count) {
    set_required_tap_count(required_tap_count);
}

void TapRecognizer::set_required_tap_count(int count) {
    required_tap_count_ = std::max(1, count);
}

void TapRecognizer::on_reset() {
    tap_count_ = 0;
    pressed_ = false;
    last_position_ = {};
    previous_release_time_ = -1.0;
}

bool TapRecognizer::keep_possible_after_release() const {
    return tap_count_ > 0 && tap_count_ < required_tap_count_;
}

void TapRecognizer::on_pointer_event(const MouseEvent& event,
                                     const GestureContext& context) {
    if (is_terminal(state()))
        reset_to_possible();

    if (event.is_wheel) return;

    if (event.is_cancelled) {
        fail();
        return;
    }

    if (!pressed_ && event.isPress()) {
        if (previous_release_time_ >= 0.0 &&
            context.timestamp_seconds - previous_release_time_ > max_interval_) {
            tap_count_ = 0;
        }
        pointer_id_ = event.pointer_id;
        pressed_ = true;
        press_position_ = event.position;
        press_time_ = context.timestamp_seconds;
        return;
    }

    if (!pressed_ || event.pointer_id != pointer_id_) return;

    if (!is_release_or_cancel(event) &&
        distance_between(event.position, press_position_) > max_movement_) {
        fail();
        return;
    }

    if (!is_release_or_cancel(event)) return;

    pressed_ = false;
    const bool quick_enough =
        context.timestamp_seconds - press_time_ <= max_press_duration_;
    const bool close_enough =
        distance_between(event.position, press_position_) <= max_movement_;
    if (!quick_enough || !close_enough) {
        fail();
        return;
    }

    ++tap_count_;
    last_position_ = event.position;
    previous_release_time_ = context.timestamp_seconds;
    if (tap_count_ >= required_tap_count_) {
        transition_to(GestureState::began);
        transition_to(GestureState::ended);
    }
}

void LongPressRecognizer::on_reset() {
    pressed_ = false;
}

void LongPressRecognizer::maybe_begin(double timestamp_seconds) {
    if (!pressed_ || state() != GestureState::possible) return;
    if (timestamp_seconds - press_time_ >= min_duration_)
        transition_to(GestureState::began);
}

void LongPressRecognizer::on_pointer_event(const MouseEvent& event,
                                           const GestureContext& context) {
    if (is_terminal(state()))
        reset_to_possible();

    if (event.is_wheel) return;

    if (event.is_cancelled) {
        cancel();
        pressed_ = false;
        return;
    }

    if (!pressed_ && event.isPress()) {
        pointer_id_ = event.pointer_id;
        pressed_ = true;
        press_position_ = event.position;
        press_time_ = context.timestamp_seconds;
        return;
    }

    if (!pressed_ || event.pointer_id != pointer_id_) return;

    if (distance_between(event.position, press_position_) > max_movement_) {
        if (state() == GestureState::possible) fail();
        else cancel();
        pressed_ = false;
        return;
    }

    maybe_begin(context.timestamp_seconds);

    if (is_release_or_cancel(event)) {
        pressed_ = false;
        if (state() == GestureState::began || state() == GestureState::changed)
            transition_to(GestureState::ended);
        else
            fail();
    }
}

void LongPressRecognizer::on_time_advanced(const GestureContext& context) {
    maybe_begin(context.timestamp_seconds);
}

bool LongPressRecognizer::wants_time_updates() const {
    return pressed_ && state() == GestureState::possible;
}

void PanRecognizer::on_reset() {
    pressed_ = false;
    translation_ = {};
    velocity_ = {};
}

void PanRecognizer::on_pointer_event(const MouseEvent& event,
                                     const GestureContext& context) {
    if (is_terminal(state()))
        reset_to_possible();

    if (event.is_wheel) return;

    if (event.is_cancelled) {
        cancel();
        pressed_ = false;
        return;
    }

    if (!pressed_ && event.isPress()) {
        pointer_id_ = event.pointer_id;
        pressed_ = true;
        start_position_ = event.position;
        last_position_ = event.position;
        last_time_ = context.timestamp_seconds;
        translation_ = {};
        velocity_ = {};
        return;
    }

    if (!pressed_ || event.pointer_id != pointer_id_) return;

    const double dt = std::max(1.0e-6, context.timestamp_seconds - last_time_);
    velocity_ = {(event.position.x - last_position_.x) / static_cast<float>(dt),
                 (event.position.y - last_position_.y) / static_cast<float>(dt)};
    translation_ = subtract(event.position, start_position_);
    last_position_ = event.position;
    last_time_ = context.timestamp_seconds;

    if (is_release_or_cancel(event)) {
        pressed_ = false;
        if (state() == GestureState::began || state() == GestureState::changed)
            transition_to(GestureState::ended);
        else
            fail();
        return;
    }

    if (state() == GestureState::possible) {
        if (magnitude(translation_) >= min_distance_)
            transition_to(GestureState::began);
    } else {
        transition_to(GestureState::changed);
    }
}

void SwipeRecognizer::on_reset() {
    pressed_ = false;
    translation_ = {};
    velocity_ = {};
}

void SwipeRecognizer::on_pointer_event(const MouseEvent& event,
                                       const GestureContext& context) {
    if (is_terminal(state()))
        reset_to_possible();

    if (event.is_wheel) return;

    if (event.is_cancelled) {
        fail();
        pressed_ = false;
        return;
    }

    if (!pressed_ && event.isPress()) {
        pointer_id_ = event.pointer_id;
        pressed_ = true;
        start_position_ = event.position;
        last_position_ = event.position;
        start_time_ = context.timestamp_seconds;
        last_time_ = context.timestamp_seconds;
        translation_ = {};
        velocity_ = {};
        return;
    }

    if (!pressed_ || event.pointer_id != pointer_id_) return;

    const double dt = std::max(1.0e-6, context.timestamp_seconds - last_time_);
    velocity_ = {(event.position.x - last_position_.x) / static_cast<float>(dt),
                 (event.position.y - last_position_.y) / static_cast<float>(dt)};
    translation_ = subtract(event.position, start_position_);
    last_position_ = event.position;
    last_time_ = context.timestamp_seconds;

    if (!is_release_or_cancel(event)) return;

    pressed_ = false;
    const double duration = std::max(1.0e-6, context.timestamp_seconds - start_time_);
    const Point average_velocity{
        translation_.x / static_cast<float>(duration),
        translation_.y / static_cast<float>(duration),
    };
    if (magnitude(translation_) >= min_distance_ &&
        magnitude(average_velocity) >= min_velocity_) {
        velocity_ = average_velocity;
        transition_to(GestureState::began);
        transition_to(GestureState::ended);
    } else {
        fail();
    }
}

void FlingRecognizer::on_reset() {
    pressed_ = false;
    translation_ = {};
    velocity_ = {};
}

void FlingRecognizer::on_pointer_event(const MouseEvent& event,
                                       const GestureContext& context) {
    if (is_terminal(state()))
        reset_to_possible();

    if (event.is_wheel) return;

    if (event.is_cancelled) {
        fail();
        pressed_ = false;
        return;
    }

    if (!pressed_ && event.isPress()) {
        pointer_id_ = event.pointer_id;
        pressed_ = true;
        start_position_ = event.position;
        last_position_ = event.position;
        start_time_ = context.timestamp_seconds;
        last_time_ = context.timestamp_seconds;
        translation_ = {};
        velocity_ = {};
        return;
    }

    if (!pressed_ || event.pointer_id != pointer_id_) return;

    const double dt = std::max(1.0e-6, context.timestamp_seconds - last_time_);
    velocity_ = {(event.position.x - last_position_.x) / static_cast<float>(dt),
                 (event.position.y - last_position_.y) / static_cast<float>(dt)};
    translation_ = subtract(event.position, start_position_);
    last_position_ = event.position;
    last_time_ = context.timestamp_seconds;

    if (!is_release_or_cancel(event)) return;

    pressed_ = false;
    const double duration = std::max(1.0e-6, context.timestamp_seconds - start_time_);
    const Point average_velocity{
        translation_.x / static_cast<float>(duration),
        translation_.y / static_cast<float>(duration),
    };
    if (duration <= max_duration_ &&
        magnitude(average_velocity) >= min_velocity_) {
        velocity_ = average_velocity;
        transition_to(GestureState::began);
        transition_to(GestureState::ended);
    } else {
        fail();
    }
}

void PinchRecognizer::on_reset() {
    touches_.clear();
    initial_distance_ = 0.0f;
    last_scale_ = 1.0f;
    scale_ = 1.0f;
    delta_scale_ = 0.0f;
    center_ = {};
}

bool PinchRecognizer::pair_metrics(float* distance, Point* center) const {
    if (touches_.size() < 2) return false;
    auto first = touches_.begin();
    auto second = std::next(first);
    const Point a = first->second.current;
    const Point b = second->second.current;
    if (distance) *distance = std::max(1.0e-6f, distance_between(a, b));
    if (center) *center = midpoint(a, b);
    return true;
}

void PinchRecognizer::finish_or_cancel(bool cancelled) {
    if (state() == GestureState::began || state() == GestureState::changed) {
        if (cancelled)
            transition_to(GestureState::cancelled);
        else
            transition_to(GestureState::ended);
    } else {
        fail();
    }
}

void PinchRecognizer::on_pointer_event(const MouseEvent& event,
                                       const GestureContext& context) {
    (void)context;
    if (is_terminal(state()))
        reset_to_possible();

    if (event.is_wheel) return;

    if (event.is_cancelled) {
        touches_.erase(event.pointer_id);
        finish_or_cancel(true);
        return;
    }

    if (event.isPress()) {
        touches_[event.pointer_id] = Touch{event.position, event.position};
        if (touches_.size() == 2) {
            pair_metrics(&initial_distance_, &center_);
            last_scale_ = 1.0f;
            scale_ = 1.0f;
            delta_scale_ = 0.0f;
        }
        return;
    }

    auto it = touches_.find(event.pointer_id);
    if (it == touches_.end()) return;
    it->second.current = event.position;

    if (is_release_or_cancel(event)) {
        touches_.erase(it);
        finish_or_cancel(false);
        return;
    }

    if (touches_.size() < 2 || initial_distance_ <= 0.0f) return;

    float distance = 0.0f;
    pair_metrics(&distance, &center_);
    scale_ = distance / initial_distance_;
    delta_scale_ = scale_ - last_scale_;
    last_scale_ = scale_;

    if (state() == GestureState::possible) {
        if (std::fabs(scale_ - 1.0f) >= min_scale_delta_)
            transition_to(GestureState::began);
    } else {
        transition_to(GestureState::changed);
    }
}

void RotateRecognizer::on_reset() {
    touches_.clear();
    initial_angle_ = 0.0f;
    last_rotation_ = 0.0f;
    rotation_ = 0.0f;
    delta_rotation_ = 0.0f;
    center_ = {};
}

bool RotateRecognizer::pair_metrics(float* angle, Point* center) const {
    if (touches_.size() < 2) return false;
    auto first = touches_.begin();
    auto second = std::next(first);
    const Point a = first->second.current;
    const Point b = second->second.current;
    if (angle) *angle = angle_between(a, b);
    if (center) *center = midpoint(a, b);
    return true;
}

void RotateRecognizer::finish_or_cancel(bool cancelled) {
    if (state() == GestureState::began || state() == GestureState::changed) {
        if (cancelled)
            transition_to(GestureState::cancelled);
        else
            transition_to(GestureState::ended);
    } else {
        fail();
    }
}

void RotateRecognizer::on_pointer_event(const MouseEvent& event,
                                        const GestureContext& context) {
    (void)context;
    if (is_terminal(state()))
        reset_to_possible();

    if (event.is_wheel) return;

    if (event.is_cancelled) {
        touches_.erase(event.pointer_id);
        finish_or_cancel(true);
        return;
    }

    if (event.isPress()) {
        touches_[event.pointer_id] = Touch{event.position, event.position};
        if (touches_.size() == 2) {
            pair_metrics(&initial_angle_, &center_);
            last_rotation_ = 0.0f;
            rotation_ = 0.0f;
            delta_rotation_ = 0.0f;
        }
        return;
    }

    auto it = touches_.find(event.pointer_id);
    if (it == touches_.end()) return;
    it->second.current = event.position;

    if (is_release_or_cancel(event)) {
        touches_.erase(it);
        finish_or_cancel(false);
        return;
    }

    if (touches_.size() < 2) return;

    float angle = 0.0f;
    pair_metrics(&angle, &center_);
    rotation_ = normalize_angle_delta(angle - initial_angle_);
    delta_rotation_ = normalize_angle_delta(rotation_ - last_rotation_);
    last_rotation_ = rotation_;

    if (state() == GestureState::possible) {
        if (std::fabs(rotation_) >= min_rotation_)
            transition_to(GestureState::began);
    } else {
        transition_to(GestureState::changed);
    }
}

GestureArbiter::PointerSession& GestureArbiter::start_session(
        View& root, const MouseEvent& root_event) {
    const int pointer_id = root_event.pointer_id;
    auto [it, inserted] = sessions_.emplace(pointer_id, PointerSession{});
    PointerSession& session = it->second;
    if (!inserted) {
        session.candidates.clear();
    }
    session.pointer_id = pointer_id;

    const Point root_pos = root_position_for(root_event);
    View* target = root.hit_test(root_pos);
    for (View* view = target; view; view = view->parent()) {
        const size_t count = view->gesture_recognizer_count();
        for (size_t i = 0; i < count; ++i) {
            if (auto* recognizer = view->gesture_recognizer_at(i)) {
                if (is_terminal(recognizer->state()))
                    recognizer->reset_to_possible();
                session.candidates.push_back(Candidate{recognizer, view, false});
            }
        }
        if (view == &root) break;
    }
    return session;
}

void GestureArbiter::feed_session(View& root, PointerSession& session,
                                  const MouseEvent& root_event,
                                  const GestureContext& context) {
    const Point root_pos = context.root_position;
    for (auto& candidate : session.candidates) {
        if (!candidate.recognizer || !candidate.owner) continue;
        if (candidate.recognizer->state() == GestureState::failed ||
            candidate.recognizer->state() == GestureState::cancelled) {
            continue;
        }
        MouseEvent local_event = root_event;
        local_event.position = root_to_local(root_pos, candidate.owner, root);
        local_event.window_position = root_pos;
        candidate.recognizer->on_pointer_event(local_event, context);
    }
}

bool GestureArbiter::active_recognizers_allow(
        const PointerSession& session,
        const Candidate& pending_candidate) const {
    auto* recognizer = pending_candidate.recognizer;
    if (!recognizer) return false;

    for (const auto& [unused, active_session] : sessions_) {
        (void)unused;
        for (const auto& candidate : active_session.candidates) {
            if (!candidate.active || !candidate.recognizer) continue;
            if (candidate.recognizer == recognizer) continue;
            if (!views_share_gesture_branch(candidate.owner, pending_candidate.owner))
                continue;
            if (!recognizer->can_recognize_simultaneously_with(*candidate.recognizer))
                return false;
        }
    }

    for (const auto& candidate : session.candidates) {
        if (!candidate.active || !candidate.recognizer) continue;
        if (candidate.recognizer == recognizer) continue;
        if (!views_share_gesture_branch(candidate.owner, pending_candidate.owner))
            continue;
        if (!recognizer->can_recognize_simultaneously_with(*candidate.recognizer))
            return false;
    }

    return true;
}

void GestureArbiter::fail_conflicting_candidates(
        const Candidate& active_candidate) {
    auto* active_recognizer = active_candidate.recognizer;
    if (!active_recognizer) return;

    for (auto& [unused, active_session] : sessions_) {
        (void)unused;
        for (auto& candidate : active_session.candidates) {
            auto* recognizer = candidate.recognizer;
            if (!recognizer || recognizer == active_recognizer) continue;
            if (candidate.active) continue;
            if (!views_share_gesture_branch(candidate.owner, active_candidate.owner))
                continue;
            if (!is_terminal(recognizer->state()) &&
                !recognizer->can_recognize_simultaneously_with(*active_recognizer)) {
                recognizer->fail();
            }
        }
    }
}

void GestureArbiter::resolve_session(PointerSession& session,
                                     const MouseEvent& root_event) {
    auto requirements_satisfied = [&](GestureRecognizer& recognizer) {
        for (const auto& candidate : session.candidates) {
            auto* other = candidate.recognizer;
            if (!other || other == &recognizer) continue;
            if (!recognizer.requires_failure_of(*other)) continue;
            if (other->state() != GestureState::failed)
                return false;
        }
        return true;
    };

    for (auto& candidate : session.candidates) {
        auto* recognizer = candidate.recognizer;
        if (!recognizer || candidate.active) continue;
        if (!is_recognized(recognizer->state())) continue;
        if (!requirements_satisfied(*recognizer)) continue;
        if (!active_recognizers_allow(session, candidate)) {
            recognizer->fail();
            continue;
        }
        candidate.active = true;
        if (candidate.owner)
            candidate.owner->set_pointer_capture(root_event.pointer_id);
        fail_conflicting_candidates(candidate);
    }

    for (auto& candidate : session.candidates) {
        if (candidate.active && candidate.recognizer)
            candidate.recognizer->dispatch_pending_callbacks();
        else if (candidate.recognizer &&
                 !(is_recognized(candidate.recognizer->state()) &&
                   !requirements_satisfied(*candidate.recognizer)))
            candidate.recognizer->clear_pending_callbacks();
    }
}

void GestureArbiter::finish_session_if_needed(PointerSession& session,
                                              const MouseEvent& root_event) {
    const bool release = is_release_or_cancel(root_event);
    for (auto& candidate : session.candidates) {
        auto* recognizer = candidate.recognizer;
        if (!recognizer) continue;
        if (candidate.active && is_terminal(recognizer->state())) {
            if (candidate.owner)
                candidate.owner->release_pointer_capture(root_event.pointer_id);
            candidate.active = false;
        }
    }

    if (!release) return;

    for (auto& candidate : session.candidates) {
        auto* recognizer = candidate.recognizer;
        if (!recognizer) continue;
        if (candidate.active && candidate.owner)
            candidate.owner->release_pointer_capture(root_event.pointer_id);
        if (!is_terminal(recognizer->state()) &&
            !(recognizer->state() == GestureState::possible &&
              recognizer->keep_possible_after_release()))
            recognizer->fail();
        candidate.active = false;
    }
}

bool GestureArbiter::handle_pointer_event(View& root, const MouseEvent& root_event,
                                          double timestamp_seconds) {
    if (root_event.is_wheel) return false;
    if (timestamp_seconds < 0.0)
        timestamp_seconds = default_timestamp_seconds();

    const bool has_session =
        sessions_.find(root_event.pointer_id) != sessions_.end();
    const bool is_press = is_press_without_session(root_event, has_session);
    if (!is_press && !has_session) return false;

    PointerSession& session = is_press
        ? start_session(root, root_event)
        : sessions_.at(root_event.pointer_id);
    if (session.candidates.empty()) {
        if (is_release_or_cancel(root_event))
            sessions_.erase(root_event.pointer_id);
        return false;
    }

    GestureContext context;
    context.timestamp_seconds = timestamp_seconds;
    context.root_position = root_position_for(root_event);

    feed_session(root, session, root_event, context);
    resolve_session(session, root_event);

    const bool consumed = !session.candidates.empty();
    const int pointer_id = session.pointer_id;
    finish_session_if_needed(session, root_event);
    if (is_release_or_cancel(root_event))
        sessions_.erase(pointer_id);
    return consumed;
}

void GestureArbiter::advance_time(View& root, double timestamp_seconds) {
    if (timestamp_seconds < 0.0)
        timestamp_seconds = default_timestamp_seconds();

    std::vector<int> pointer_ids;
    pointer_ids.reserve(sessions_.size());
    for (const auto& [pointer_id, unused] : sessions_) {
        (void)unused;
        pointer_ids.push_back(pointer_id);
    }

    for (int pointer_id : pointer_ids) {
        auto it = sessions_.find(pointer_id);
        if (it == sessions_.end()) continue;
        auto& session = it->second;
        GestureContext context;
        context.timestamp_seconds = timestamp_seconds;
        for (auto& candidate : session.candidates) {
            if (!candidate.recognizer) continue;
            candidate.recognizer->on_time_advanced(context);
        }
        MouseEvent synthetic;
        synthetic.pointer_id = pointer_id;
        resolve_session(session, synthetic);
        (void)root;
    }
}

bool GestureArbiter::wants_time_updates() const {
    for (const auto& [unused, session] : sessions_) {
        (void)unused;
        for (const auto& candidate : session.candidates) {
            if (candidate.recognizer && candidate.recognizer->wants_time_updates())
                return true;
        }
    }
    return false;
}

void GestureArbiter::reset() {
    for (auto& [unused, session] : sessions_) {
        (void)unused;
        for (auto& candidate : session.candidates) {
            if (!candidate.recognizer) continue;
            if (candidate.active)
                candidate.recognizer->cancel();
            else
                candidate.recognizer->fail();
            candidate.recognizer->dispatch_pending_callbacks();
            if (candidate.owner)
                candidate.owner->release_pointer_capture(session.pointer_id);
        }
    }
    sessions_.clear();
}

}  // namespace pulp::view
