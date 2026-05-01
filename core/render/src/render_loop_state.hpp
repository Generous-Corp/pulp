#pragma once

#include <atomic>

namespace pulp::render {

class RenderLoopState {
public:
    bool start() {
        const bool was_running = running_.exchange(true, std::memory_order_acq_rel);
        needs_frame_.store(true, std::memory_order_release);
        return !was_running;
    }

    bool stop() {
        const bool was_running = running_.exchange(false, std::memory_order_acq_rel);
        needs_frame_.store(false, std::memory_order_release);
        return was_running;
    }

    void request_frame() {
        if (is_running()) {
            needs_frame_.store(true, std::memory_order_release);
        }
    }

    bool consume_frame_request() {
        return is_running() && needs_frame_.exchange(false, std::memory_order_acq_rel);
    }

    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> needs_frame_{false};
};

} // namespace pulp::render
