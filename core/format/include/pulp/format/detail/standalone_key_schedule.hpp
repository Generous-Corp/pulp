#pragma once

#include <pulp/format/detail/standalone_key_sequence.hpp>

#include <cstddef>
#include <utility>
#include <vector>

namespace pulp::format::detail {

/// Frame-driven schedule for the synthetic key sequence.
///
/// Presses and captures alternate, each separated by the full frame delay, so
/// every press gets an undisturbed settle window before it is photographed and
/// before the next press lands. Keeping the schedule as a pure state machine —
/// no window, no AppKit — lets the ordering be tested directly; the ordering is
/// the part that decides whether a captured frame actually shows the effect of
/// the press it is named after.
///
/// Timeline for delay D and N steps:
///   frame 1*D            -> capture_initial   (the UI before any key)
///   frame 2*D            -> press step 0
///   frame 3*D            -> capture step 0
///   frame 4*D            -> press step 1      ... and so on
///   frame (2N+2)*D       -> finish (one full slot AFTER the last capture)
class KeySequenceSchedule {
public:
    enum class Action { wait, capture_initial, press, capture, finish };

    struct Tick {
        Action action = Action::wait;
        /// Index into the step list; meaningful for press and capture.
        std::size_t index = 0;
    };

    KeySequenceSchedule(std::vector<KeySequenceStep> steps, int frame_delay)
        : steps_(std::move(steps)),
          delay_(frame_delay > 0 ? frame_delay : 1) {
        next_action_frame_ = delay_;
    }

    /// Advance exactly one frame and report what the host should do now.
    Tick tick() {
        if (done_) return {Action::wait, 0};
        ++frame_;
        if (frame_ < next_action_frame_) return {Action::wait, 0};
        next_action_frame_ += delay_;

        if (!captured_initial_) {
            captured_initial_ = true;
            if (steps_.empty()) {
                done_ = true;
                return {Action::finish, 0};
            }
            return {Action::capture_initial, 0};
        }

        if (awaiting_capture_) {
            awaiting_capture_ = false;
            const std::size_t captured = cursor_;
            ++cursor_;
            // Deliberately NOT done here: the last capture is followed by one
            // more slot that reports finish, so a caller which closes the
            // window on finish never closes it in the same frame it is still
            // reading pixels out of.
            return {Action::capture, captured};
        }

        if (cursor_ < steps_.size()) {
            awaiting_capture_ = true;
            return {Action::press, cursor_};
        }

        done_ = true;
        return {Action::finish, 0};
    }

    bool done() const { return done_; }
    const std::vector<KeySequenceStep>& steps() const { return steps_; }
    /// Total frames the whole sequence occupies, for callers that must arm a
    /// later one-shot (a screenshot, a close) after the keys are finished.
    int total_frames() const {
        // 1 initial capture + 2 slots per step (press, capture) + 1 finish.
        return delay_ * (2 + 2 * static_cast<int>(steps_.size()));
    }

private:
    std::vector<KeySequenceStep> steps_;
    int delay_ = 1;
    int frame_ = 0;
    int next_action_frame_ = 1;  // reset to delay_ by the constructor
    std::size_t cursor_ = 0;
    bool captured_initial_ = false;
    bool awaiting_capture_ = false;
    bool done_ = false;
};

}  // namespace pulp::format::detail
