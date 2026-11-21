#include "warm_up_test_access.hpp"

#include <pulp/midi/device.hpp>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace pulp::midi {

namespace {

// The worker is detached and may outlive static destruction, so the state it
// touches is never destroyed.
struct WarmUpState {
    std::mutex mu;
    std::condition_variable cv;
    bool started = false;
    bool done = false;
};

WarmUpState& state() {
    static auto* s = new WarmUpState;
    return *s;
}

void platform_probe() {
#if defined(__APPLE__)
    // The first port count connects to the MIDI server; the endpoint property
    // reads inside enumerate_*() are the next cold calls.
    auto system = create_midi_system();
    (void)system->enumerate_inputs();
    (void)system->enumerate_outputs();
#endif
}

} // namespace

namespace detail {

bool start_warm_up(std::function<void()> work) {
    auto& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (s.started)
            return false;
        s.started = true;
        s.done = false;
    }
    try {
        std::thread([work = std::move(work)] {
            try {
                if (work)
                    work();
            } catch (...) {
                // A failed warm-up only means the first real call pays the cost.
            }
            auto& st = state();
            {
                std::lock_guard<std::mutex> lk(st.mu);
                st.done = true;
            }
            st.cv.notify_all();
        }).detach();
    } catch (...) {
        // No thread available: nothing to warm, nothing to wait for.
        {
            std::lock_guard<std::mutex> lk(s.mu);
            s.done = true;
        }
        s.cv.notify_all();
    }
    return true;
}

void reset_warm_up_for_testing() noexcept {
    wait_for_warm_up();
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    s.started = false;
    s.done = false;
}

} // namespace detail

void warm_up() {
    (void)detail::start_warm_up(platform_probe);
}

bool warm_up_done() noexcept {
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.mu);
    return s.started && s.done;
}

void wait_for_warm_up() noexcept {
    auto& s = state();
    std::unique_lock<std::mutex> lk(s.mu);
    s.cv.wait(lk, [&] { return !s.started || s.done; });
}

} // namespace pulp::midi
