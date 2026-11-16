#pragma once

// A snap timer a test drives by hand: the settle time of a contract session's
// snapped-size request elapses only when the test calls fire().

#include <pulp/format/contract_editor_session.hpp>

#include <functional>
#include <memory>
#include <utility>

namespace pulp::test_support {

/// Drives a session's snapped-size debounce without a clock: every start() is
/// counted, and nothing fires until the test calls fire().
class ScopedManualSnapTimer {
  public:
    struct State {
        int starts = 0;
        int stops = 0;
        int last_delay_ms = 0;
        bool armed = false;
        std::function<void()> callback;
    };
    ScopedManualSnapTimer() {
        format::ContractEditorSession::set_snap_timer_factory_for_testing(
            [state = state_] { return std::make_unique<Timer>(state); });
    }
    ~ScopedManualSnapTimer() {
        format::ContractEditorSession::set_snap_timer_factory_for_testing({});
    }
    ScopedManualSnapTimer(const ScopedManualSnapTimer&) = delete;
    ScopedManualSnapTimer& operator=(const ScopedManualSnapTimer&) = delete;

    int starts() const {
        return state_->starts;
    }
    bool armed() const {
        return state_->armed;
    }
    int delay_ms() const {
        return state_->last_delay_ms;
    }
    /// The settle time elapses: the pending request, if any, is made now.
    void fire() {
        if (!state_->armed)
            return;
        auto callback = state_->callback;
        callback();
    }

  private:
    class Timer final : public format::ContractEditorSession::SnapTimer {
      public:
        explicit Timer(std::shared_ptr<State> state) : state_(std::move(state)) {}
        void start(int delay_ms, std::function<void()> fire) override {
            ++state_->starts;
            state_->last_delay_ms = delay_ms;
            state_->armed = true;
            state_->callback = std::move(fire);
        }
        void stop() override {
            if (state_->armed)
                ++state_->stops;
            state_->armed = false;
        }

      private:
        std::shared_ptr<State> state_;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

} // namespace pulp::test_support
