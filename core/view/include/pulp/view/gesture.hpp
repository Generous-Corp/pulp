#pragma once

#include <pulp/view/input_events.hpp>

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

namespace pulp::view {

class View;

enum class GestureState {
    possible,
    began,
    changed,
    ended,
    cancelled,
    failed,
};

struct GestureContext {
    double timestamp_seconds = 0.0;
    Point root_position{};
};

class GestureRecognizer {
public:
    using Callback = std::function<void(GestureRecognizer&)>;

    virtual ~GestureRecognizer() = default;

    GestureState state() const { return state_; }
    View* owner() const { return owner_; }

    void require_to_fail(GestureRecognizer& other);
    void allow_simultaneous_with(GestureRecognizer& other);

    bool requires_failure_of(const GestureRecognizer& other) const;
    bool can_recognize_simultaneously_with(const GestureRecognizer& other) const;

    Callback on_began;
    Callback on_changed;
    Callback on_ended;
    Callback on_cancelled;

protected:
    friend class View;
    friend class GestureArbiter;

    virtual void on_pointer_event(const MouseEvent& event,
                                  const GestureContext& context) = 0;
    virtual void on_time_advanced(const GestureContext& context) {
        (void)context;
    }
    virtual void on_reset() {}
    virtual bool keep_possible_after_release() const { return false; }
    virtual bool wants_time_updates() const { return false; }

    void transition_to(GestureState state);
    void reset_to_possible();
    void fail();
    void cancel();

private:
    void set_owner(View* owner) { owner_ = owner; }
    bool has_pending_callbacks() const { return !pending_callbacks_.empty(); }
    void clear_pending_callbacks() { pending_callbacks_.clear(); }
    void dispatch_pending_callbacks();

    View* owner_ = nullptr;
    GestureState state_ = GestureState::possible;
    std::vector<GestureRecognizer*> require_failures_;
    std::vector<GestureRecognizer*> simultaneous_;
    std::vector<GestureState> pending_callbacks_;
};

class TapRecognizer final : public GestureRecognizer {
public:
    explicit TapRecognizer(int required_tap_count = 1);

    int required_tap_count() const { return required_tap_count_; }
    Point position() const { return last_position_; }
    void set_required_tap_count(int count);
    void set_max_movement(float points) { max_movement_ = points; }
    void set_max_press_duration(double seconds) { max_press_duration_ = seconds; }
    void set_max_interval(double seconds) { max_interval_ = seconds; }

protected:
    void on_pointer_event(const MouseEvent& event,
                          const GestureContext& context) override;
    void on_reset() override;
    bool keep_possible_after_release() const override;

private:
    int required_tap_count_ = 1;
    int tap_count_ = 0;
    int pointer_id_ = 0;
    bool pressed_ = false;
    Point press_position_{};
    Point last_position_{};
    double press_time_ = 0.0;
    double previous_release_time_ = -1.0;
    float max_movement_ = 8.0f;
    double max_press_duration_ = 0.35;
    double max_interval_ = 0.30;
};

class LongPressRecognizer final : public GestureRecognizer {
public:
    Point position() const { return press_position_; }
    void set_min_duration(double seconds) { min_duration_ = seconds; }
    void set_max_movement(float points) { max_movement_ = points; }

protected:
    void on_pointer_event(const MouseEvent& event,
                          const GestureContext& context) override;
    void on_time_advanced(const GestureContext& context) override;
    void on_reset() override;
    bool wants_time_updates() const override;

private:
    void maybe_begin(double timestamp_seconds);

    int pointer_id_ = 0;
    bool pressed_ = false;
    Point press_position_{};
    double press_time_ = 0.0;
    float max_movement_ = 8.0f;
    double min_duration_ = 0.50;
};

class PanRecognizer final : public GestureRecognizer {
public:
    void set_min_distance(float points) { min_distance_ = points; }

    Point translation() const { return translation_; }
    Point velocity() const { return velocity_; }

protected:
    void on_pointer_event(const MouseEvent& event,
                          const GestureContext& context) override;
    void on_reset() override;

private:
    int pointer_id_ = 0;
    bool pressed_ = false;
    Point start_position_{};
    Point last_position_{};
    double last_time_ = 0.0;
    Point translation_{};
    Point velocity_{};
    float min_distance_ = 5.0f;
};

class SwipeRecognizer final : public GestureRecognizer {
public:
    void set_min_distance(float points) { min_distance_ = points; }
    void set_min_velocity(float points_per_second) { min_velocity_ = points_per_second; }

    Point translation() const { return translation_; }
    Point velocity() const { return velocity_; }

protected:
    void on_pointer_event(const MouseEvent& event,
                          const GestureContext& context) override;
    void on_reset() override;

private:
    int pointer_id_ = 0;
    bool pressed_ = false;
    Point start_position_{};
    Point last_position_{};
    double start_time_ = 0.0;
    double last_time_ = 0.0;
    Point translation_{};
    Point velocity_{};
    float min_distance_ = 30.0f;
    float min_velocity_ = 300.0f;
};

class FlingRecognizer final : public GestureRecognizer {
public:
    void set_min_velocity(float points_per_second) { min_velocity_ = points_per_second; }
    void set_max_duration(double seconds) { max_duration_ = seconds; }

    Point translation() const { return translation_; }
    Point velocity() const { return velocity_; }

protected:
    void on_pointer_event(const MouseEvent& event,
                          const GestureContext& context) override;
    void on_reset() override;

private:
    int pointer_id_ = 0;
    bool pressed_ = false;
    Point start_position_{};
    Point last_position_{};
    double start_time_ = 0.0;
    double last_time_ = 0.0;
    Point translation_{};
    Point velocity_{};
    float min_velocity_ = 800.0f;
    double max_duration_ = 0.50;
};

class PinchRecognizer final : public GestureRecognizer {
public:
    void set_min_scale_delta(float delta) { min_scale_delta_ = delta; }

    float scale() const { return scale_; }
    float delta_scale() const { return delta_scale_; }
    Point center() const { return center_; }

protected:
    void on_pointer_event(const MouseEvent& event,
                          const GestureContext& context) override;
    void on_reset() override;

private:
    struct Touch {
        Point start{};
        Point current{};
    };

    bool pair_metrics(float* distance, Point* center) const;
    void finish_or_cancel(bool cancelled);

    std::unordered_map<int, Touch> touches_;
    float initial_distance_ = 0.0f;
    float last_scale_ = 1.0f;
    float scale_ = 1.0f;
    float delta_scale_ = 0.0f;
    Point center_{};
    float min_scale_delta_ = 0.025f;
};

class RotateRecognizer final : public GestureRecognizer {
public:
    void set_min_rotation(float radians) { min_rotation_ = radians; }

    float rotation() const { return rotation_; }
    float delta_rotation() const { return delta_rotation_; }
    Point center() const { return center_; }

protected:
    void on_pointer_event(const MouseEvent& event,
                          const GestureContext& context) override;
    void on_reset() override;

private:
    struct Touch {
        Point start{};
        Point current{};
    };

    bool pair_metrics(float* angle, Point* center) const;
    void finish_or_cancel(bool cancelled);

    std::unordered_map<int, Touch> touches_;
    float initial_angle_ = 0.0f;
    float last_rotation_ = 0.0f;
    float rotation_ = 0.0f;
    float delta_rotation_ = 0.0f;
    Point center_{};
    float min_rotation_ = 0.035f;
};

class GestureArbiter {
public:
    bool handle_pointer_event(View& root, const MouseEvent& root_event,
                              double timestamp_seconds = -1.0);
    void advance_time(View& root, double timestamp_seconds = -1.0);
    bool wants_time_updates() const;
    void reset();

private:
    struct Candidate {
        GestureRecognizer* recognizer = nullptr;
        View* owner = nullptr;
        bool active = false;
    };

    struct PointerSession {
        int pointer_id = 0;
        std::vector<Candidate> candidates;
    };

    using SessionMap = std::unordered_map<int, PointerSession>;

    PointerSession& start_session(View& root, const MouseEvent& root_event);
    void feed_session(View& root, PointerSession& session,
                      const MouseEvent& root_event,
                      const GestureContext& context);
    void resolve_session(PointerSession& session, const MouseEvent& root_event);
    void finish_session_if_needed(PointerSession& session,
                                  const MouseEvent& root_event);
    bool active_recognizers_allow(const PointerSession& session,
                                  const Candidate& candidate) const;
    void fail_conflicting_candidates(const Candidate& active_candidate);

    SessionMap sessions_;
};

}  // namespace pulp::view
