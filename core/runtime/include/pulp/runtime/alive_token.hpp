#pragma once

#include <atomic>
#include <memory>

namespace pulp::runtime {

/// Shared, fail-closed lifetime signal for callbacks that may outlive an owner.
///
/// The owner keeps the AliveToken and callbacks capture a Handle. Destroying or
/// explicitly retiring the token flips every handle to false before the owner's
/// referenced state is released. A handle never extends the owner's lifetime;
/// it only makes the decision to avoid dereferencing it safe.
class AliveToken {
public:
    using Handle = std::shared_ptr<std::atomic<bool>>;

    AliveToken() : state_(std::make_shared<std::atomic<bool>>(true)) {}
    ~AliveToken() { retire(); }

    AliveToken(const AliveToken&) = delete;
    AliveToken& operator=(const AliveToken&) = delete;
    AliveToken(AliveToken&&) = delete;
    AliveToken& operator=(AliveToken&&) = delete;

    Handle capture() const noexcept { return state_; }

    void retire() noexcept {
        if (state_) state_->store(false, std::memory_order_release);
    }

    void reset() {
        retire();
        state_ = std::make_shared<std::atomic<bool>>(true);
    }

    static bool is_alive(const Handle& handle) noexcept {
        return handle && handle->load(std::memory_order_acquire);
    }

private:
    Handle state_;
};

} // namespace pulp::runtime
