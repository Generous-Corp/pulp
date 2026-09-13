#include <pulp/view/gesture.hpp>
#include <pulp/view/pointer_dispatch.hpp>
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
    return point_to_local(root_position, view, &root);
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

void GestureRecognizer::remove_relationships_to(
    const GestureRecognizer& other) noexcept {
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

std::shared_ptr<GestureArbiter::PointerSession> GestureArbiter::start_session(
        View& root, const MouseEvent& root_event) {
    const int pointer_id = root_event.pointer_id;
    auto [it, inserted] = sessions_.emplace(pointer_id, nullptr);
    if (inserted || !it->second)
        it->second = std::make_shared<PointerSession>();
    std::shared_ptr<PointerSession> session_ptr = it->second;
    PointerSession& session = *session_ptr;
    session.candidates.clear();
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
    return session_ptr;
}

bool GestureArbiter::session_is_live(const PointerSession& session) const {
    auto it = sessions_.find(session.pointer_id);
    return it != sessions_.end() && it->second.get() == &session;
}

void GestureArbiter::feed_session(View& root, PointerSession& session,
                                  const MouseEvent& root_event,
                                  const GestureContext& context) {
    const Point root_pos = context.root_position;
    for (std::size_t i = 0; i < session.candidates.size(); ++i) {
        Candidate candidate = session.candidates[i];
        if (!candidate.recognizer || !candidate.owner) continue;
        if (candidate.recognizer->state() == GestureState::failed ||
            candidate.recognizer->state() == GestureState::cancelled) {
            continue;
        }
        MouseEvent local_event = root_event;
        local_event.position = root_to_local(root_pos, candidate.owner, root);
        local_event.window_position = root_pos;
        candidate.recognizer->on_pointer_event(local_event, context);
        if (!session_is_live(session)) return;
    }
}

bool GestureArbiter::active_recognizers_allow(
        const PointerSession& session,
        const Candidate& pending_candidate) const {
    auto* recognizer = pending_candidate.recognizer;
    if (!recognizer) return false;

    for (const auto& [unused, active_session] : sessions_) {
        (void)unused;
        if (!active_session) continue;
        for (const auto& candidate : active_session->candidates) {
            if (!candidate.active || !candidate.recognizer) continue;
            if (candidate.recognizer == recognizer) continue;
            if (!views_share_gesture_branch(candidate.owner, pending_candidate.owner))
                continue;
            if (!recognizer->can_recognize_simultaneously_with(*candidate.recognizer))
                return false;
        }
    }

    for (std::size_t i = 0; i < session.candidates.size(); ++i) {
        const Candidate candidate = session.candidates[i];
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

    // Collect first, fail second. `fail()` runs user code that can start or
    // end a session, which would invalidate a map iterator held across it.
    std::vector<std::pair<int, std::size_t>> doomed;
    for (auto& [unused, active_session] : sessions_) {
        (void)unused;
        if (!active_session) continue;
        for (auto& candidate : active_session->candidates) {
            auto* recognizer = candidate.recognizer;
            if (!recognizer || recognizer == active_recognizer) continue;
            if (candidate.active) continue;
            if (!views_share_gesture_branch(candidate.owner, active_candidate.owner))
                continue;
            if (!is_terminal(recognizer->state()) &&
                !recognizer->can_recognize_simultaneously_with(*active_recognizer)) {
                doomed.emplace_back(active_session->pointer_id,
                                    static_cast<std::size_t>(
                                        &candidate - active_session->candidates.data()));
            }
        }
    }
    // Re-resolve each entry against the live map: an earlier fail() may have
    // torn its session down, and a recognizer read from a dead session is a
    // dangling pointer.
    for (const auto& [pointer_id, index] : doomed) {
        auto it = sessions_.find(pointer_id);
        if (it == sessions_.end() || !it->second) continue;
        auto& candidates = it->second->candidates;
        if (index >= candidates.size()) continue;
        auto* recognizer = candidates[index].recognizer;
        if (!recognizer || recognizer == active_recognizer) continue;
        if (candidates[index].active) continue;
        if (!is_terminal(recognizer->state()))
            recognizer->fail();
    }
}

void GestureArbiter::resolve_session(PointerSession& session,
                                     const MouseEvent& root_event) {
    auto requirements_satisfied = [&](GestureRecognizer& recognizer) {
        for (std::size_t i = 0; i < session.candidates.size(); ++i) {
            auto* other = session.candidates[i].recognizer;
            if (!other || other == &recognizer) continue;
            if (!recognizer.requires_failure_of(*other)) continue;
            if (other->state() != GestureState::failed)
                return false;
        }
        return true;
    };

    // Index-based, size re-read each step, liveness re-checked after every
    // call into user code: a recognizer callback can re-enter dispatch and
    // both refill this vector and erase the session out from under us.
    for (std::size_t i = 0; i < session.candidates.size(); ++i) {
        auto* recognizer = session.candidates[i].recognizer;
        if (!recognizer || session.candidates[i].active) continue;
        if (!is_recognized(recognizer->state())) continue;
        if (!requirements_satisfied(*recognizer)) continue;
        if (!active_recognizers_allow(session, session.candidates[i])) {
            recognizer->fail();
            if (!session_is_live(session)) return;
            continue;
        }
        session.candidates[i].active = true;
        if (auto* owner = session.candidates[i].owner) {
            owner->set_pointer_capture(root_event.pointer_id);
            if (!session_is_live(session)) return;
            if (i >= session.candidates.size()) return;
        }
        fail_conflicting_candidates(session.candidates[i]);
        if (!session_is_live(session)) return;
    }

    for (std::size_t i = 0; i < session.candidates.size(); ++i) {
        auto* recognizer = session.candidates[i].recognizer;
        if (session.candidates[i].active && recognizer)
            recognizer->dispatch_pending_callbacks();
        else if (recognizer &&
                 !(is_recognized(recognizer->state()) &&
                   !requirements_satisfied(*recognizer)))
            recognizer->clear_pending_callbacks();
        if (!session_is_live(session)) return;
    }
}

void GestureArbiter::finish_session_if_needed(PointerSession& session,
                                              const MouseEvent& root_event) {
    const bool release = is_release_or_cancel(root_event);
    for (std::size_t i = 0; i < session.candidates.size(); ++i) {
        auto* recognizer = session.candidates[i].recognizer;
        if (!recognizer) continue;
        if (session.candidates[i].active && is_terminal(recognizer->state())) {
            auto* owner = session.candidates[i].owner;
            session.candidates[i].active = false;
            if (owner) {
                owner->release_pointer_capture(root_event.pointer_id);
                if (!session_is_live(session)) return;
            }
        }
    }

    if (!release) return;

    for (std::size_t i = 0; i < session.candidates.size(); ++i) {
        auto* recognizer = session.candidates[i].recognizer;
        if (!recognizer) continue;
        auto* owner = session.candidates[i].owner;
        const bool was_active = session.candidates[i].active;
        session.candidates[i].active = false;
        if (was_active && owner) {
            owner->release_pointer_capture(root_event.pointer_id);
            if (!session_is_live(session)) return;
        }
        if (!is_terminal(recognizer->state()) &&
            !(recognizer->state() == GestureState::possible &&
              recognizer->keep_possible_after_release()))
            recognizer->fail();
        if (!session_is_live(session)) return;
    }
}

bool GestureArbiter::handle_pointer_event(View& root, const MouseEvent& root_event,
                                          double timestamp_seconds) {
    // Cleared before every early-out, including the wheel one: a stale claim
    // from the previous pointer event would otherwise survive into the next
    // query. The host happens to short-circuit on the dispatch return today,
    // so it never reads it — which is exactly what would make this a quiet
    // trap for the next caller.
    claimed_pointer_ = false;
    if (root_event.is_wheel) return false;
    if (timestamp_seconds < 0.0)
        timestamp_seconds = default_timestamp_seconds();

    const bool has_session =
        sessions_.find(root_event.pointer_id) != sessions_.end();
    const bool is_press = is_press_without_session(root_event, has_session);
    if (!is_press && !has_session) return false;

    // Owning the node keeps it alive for this whole frame even if a recognizer
    // callback re-enters dispatch and erases the map entry.
    std::shared_ptr<PointerSession> session_ptr =
        is_press ? start_session(root, root_event)
                 : sessions_.at(root_event.pointer_id);
    if (!session_ptr) return false;
    PointerSession& session = *session_ptr;
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

    // `consumed` says only that a recognizer EXISTS on this view's ancestor
    // chain — candidates are never pruned within a session, a failing
    // recognizer merely changes state. `claimed` says one actually took the
    // pointer. A host that bails on mere candidacy makes any control carrying
    // a recognizer permanently undraggable, because normal delivery never runs.
    const bool consumed = !session.candidates.empty();
    claimed_pointer_ = std::any_of(
        session.candidates.begin(), session.candidates.end(),
        [](const Candidate& candidate) { return candidate.active; });
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
        if (it == sessions_.end() || !it->second) continue;
        std::shared_ptr<PointerSession> session_ptr = it->second;
        PointerSession& session = *session_ptr;
        GestureContext context;
        context.timestamp_seconds = timestamp_seconds;
        for (std::size_t i = 0; i < session.candidates.size(); ++i) {
            auto* recognizer = session.candidates[i].recognizer;
            if (!recognizer) continue;
            recognizer->on_time_advanced(context);
            if (!session_is_live(session)) break;
        }
        if (!session_is_live(session)) continue;
        MouseEvent synthetic;
        synthetic.pointer_id = pointer_id;
        resolve_session(session, synthetic);
        (void)root;
    }
}

bool GestureArbiter::wants_time_updates() const {
    for (const auto& [unused, session] : sessions_) {
        (void)unused;
        if (!session) continue;
        for (const auto& candidate : session->candidates) {
            if (candidate.recognizer && candidate.recognizer->wants_time_updates())
                return true;
        }
    }
    return false;
}

void GestureArbiter::reset() {
    // Detach the map first, then tear the sessions down. The callbacks below
    // run user code that may start a new session; anything it creates belongs
    // to the fresh map, not to this teardown.
    SessionMap sessions;
    sessions.swap(sessions_);
    for (auto& [unused, session] : sessions) {
        (void)unused;
        if (!session) continue;
        for (std::size_t i = 0; i < session->candidates.size(); ++i) {
            auto candidate = session->candidates[i];
            if (!candidate.recognizer) continue;
            if (candidate.active)
                candidate.recognizer->cancel();
            else
                candidate.recognizer->fail();
            candidate.recognizer->dispatch_pending_callbacks();
            if (candidate.owner)
                candidate.owner->release_pointer_capture(session->pointer_id);
        }
    }
}

void GestureArbiter::abandon() noexcept {
    sessions_.clear();
}

}  // namespace pulp::view
