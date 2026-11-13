#pragma once

/// @file sysex_send_gate.hpp
/// Bounds how many System Exclusive messages a backend may have handed to the
/// OS and not yet seen complete. A send past the bound is refused (the caller
/// gets `false`), never dropped silently and never queued without limit. The
/// count is atomic and lock-free, so it can be shared with completion
/// callbacks that run on an OS thread after the output is gone.

#include <atomic>

namespace pulp::midi {

class SysexSendGate {
  public:
    explicit SysexSendGate(int max_in_flight) noexcept : max_(max_in_flight) {}

    /// Take a slot. False, with nothing changed, when `max_in_flight` are
    /// already outstanding.
    bool try_acquire() noexcept {
        int current = in_flight_.load(std::memory_order_relaxed);
        while (current < max_) {
            if (in_flight_.compare_exchange_weak(current, current + 1, std::memory_order_acq_rel,
                                                 std::memory_order_relaxed))
                return true;
        }
        return false;
    }

    /// Give a slot back: the message completed, or the backend refused it.
    void release() noexcept {
        in_flight_.fetch_sub(1, std::memory_order_acq_rel);
    }

    int in_flight() const noexcept {
        return in_flight_.load(std::memory_order_acquire);
    }
    int max_in_flight() const noexcept {
        return max_;
    }

  private:
    std::atomic<int> in_flight_{0};
    int max_;
};

} // namespace pulp::midi
