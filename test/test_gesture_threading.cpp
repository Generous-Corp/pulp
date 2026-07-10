// Gesture thread-safety (WS-4 / G5).
//
// StateStore::begin_gesture/end_gesture forward to host beginEdit/endEdit-style
// undo grouping, which is MAIN-THREAD-ONLY (VST3 beginEdit, AU
// AudioUnitEvent, CLAP gesture events). A background writer — e.g. a MIDI-learn
// engine setting a mapped parameter from a timer thread — must route the whole
// begin->set->end gesture through StateStore::run_gesture_on_main(), which
// marshals it onto the host main thread via MainThreadDispatcher. This pins:
//   1. run_gesture_on_main runs inline when no host backend is registered.
//   2. run_gesture_on_main from a background thread lands the gesture on the
//      main thread, in order, with no off-main violation counted.
//   3. Bypassing it (a raw off-main begin_gesture with a backend live) is
//      detected via gesture_thread_violation_count (Release-observable).
#include <catch2/catch_test_macros.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/state/binding.hpp>
#include <pulp/state/store.hpp>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

using namespace pulp;

namespace {

// A test double for the process-wide main-thread backend. One dedicated worker
// thread plays the role of the "host main thread": posted tasks run there, and
// is_main_thread() is true only on that thread.
class FakeMainThread {
public:
    FakeMainThread() {
        worker_ = std::thread([this] {
            main_id_ = std::this_thread::get_id();
            id_ready_.store(true, std::memory_order_release);
            for (;;) {
                std::function<void()> task;
                {
                    std::unique_lock lock(mu_);
                    cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
                    if (stop_ && queue_.empty()) return;
                    task = std::move(queue_.front());
                    queue_.pop_front();
                }
                task();
                ran_.fetch_add(1, std::memory_order_acq_rel);
            }
        });
        while (!id_ready_.load(std::memory_order_acquire))
            std::this_thread::yield();

        events::MainThreadDispatcher::Backend backend;
        backend.post = [this](events::Task t) {
            {
                std::lock_guard lock(mu_);
                queue_.push_back(std::move(t));
            }
            cv_.notify_one();
            return true;
        };
        backend.is_main_thread = [this] {
            return std::this_thread::get_id() == main_id_;
        };
        token_ = events::MainThreadDispatcher::register_backend(std::move(backend));
    }

    ~FakeMainThread() {
        events::MainThreadDispatcher::unregister_backend(token_);
        {
            std::lock_guard lock(mu_);
            stop_ = true;
        }
        cv_.notify_one();
        worker_.join();
    }

    std::thread::id main_id() const { return main_id_; }

    // Block until at least `n` posted tasks have executed.
    void wait_for_ran(std::size_t n) {
        while (ran_.load(std::memory_order_acquire) < n)
            std::this_thread::yield();
    }

private:
    std::thread worker_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> queue_;
    bool stop_ = false;
    std::atomic<bool> id_ready_{false};
    std::atomic<std::size_t> ran_{0};
    std::thread::id main_id_{};
    events::MainThreadDispatcher::Token token_{};
};

}  // namespace

TEST_CASE("run_gesture_on_main runs inline with no host backend", "[state][gesture][thread]") {
    // Headless: no MainThreadDispatcher backend. The gesture must run inline on
    // the calling thread so tests and non-hosted paths still work.
    REQUIRE_FALSE(events::MainThreadDispatcher::has_backend());

    state::StateStore store;
    store.add_parameter({.id = 1, .name = "Gain", .range = {0.0f, 1.0f, 0.0f, 0.0f}});

    std::thread::id ran_on{};
    int begins = 0, ends = 0;
    store.set_gesture_callbacks([&](state::ParamID) { ++begins; },
                                [&](state::ParamID) { ++ends; });

    store.run_gesture_on_main([&] {
        ran_on = std::this_thread::get_id();
        store.begin_gesture(1);
        store.set_value(1, 0.7f);
        store.end_gesture(1);
    });

    REQUIRE(ran_on == std::this_thread::get_id());
    REQUIRE(begins == 1);
    REQUIRE(ends == 1);
    REQUIRE(store.get_value(1) == 0.7f);
    REQUIRE(store.gesture_thread_violation_count() == 0);
}

TEST_CASE("run_gesture_on_main marshals a background gesture onto the main thread",
          "[state][gesture][thread]") {
    FakeMainThread host;

    state::StateStore store;
    store.add_parameter({.id = 1, .name = "Gain", .range = {0.0f, 1.0f, 0.0f, 0.0f}});

    std::atomic<std::thread::id> begin_thread{};
    std::atomic<std::thread::id> end_thread{};
    std::atomic<int> begins{0}, ends{0};
    store.set_gesture_callbacks(
        [&](state::ParamID) { begin_thread = std::this_thread::get_id(); ++begins; },
        [&](state::ParamID) { end_thread = std::this_thread::get_id(); ++ends; });

    // A background "MIDI-learn" thread sets the mapped parameter.
    std::thread bg([&] {
        REQUIRE(std::this_thread::get_id() != host.main_id());
        store.run_gesture_on_main([&] {
            store.begin_gesture(1);
            store.set_value(1, 0.42f);
            store.end_gesture(1);
        });
    });
    bg.join();

    host.wait_for_ran(1);  // the posted gesture task ran on the fake main thread

    REQUIRE(begins.load() == 1);
    REQUIRE(ends.load() == 1);
    // The host undo-grouping calls fired on the MAIN thread, not the bg thread.
    REQUIRE(begin_thread.load() == host.main_id());
    REQUIRE(end_thread.load() == host.main_id());
    REQUIRE(store.get_value(1) == 0.42f);
    // Because the gesture ran on the main thread, nothing was flagged.
    REQUIRE(store.gesture_thread_violation_count() == 0);
}

TEST_CASE("Binding::set_from_background marshals through the store",
          "[state][gesture][thread]") {
    FakeMainThread host;

    state::StateStore store;
    store.add_parameter({.id = 7, .name = "Cutoff", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    std::atomic<std::thread::id> begin_thread{};
    std::atomic<int> begins{0};
    store.set_gesture_callbacks(
        [&](state::ParamID) { begin_thread = std::this_thread::get_id(); ++begins; },
        [&](state::ParamID) {});

    state::Binding binding(store, 7);
    std::thread bg([&] { binding.set_from_background(0.9f); });
    bg.join();
    host.wait_for_ran(1);

    REQUIRE(begins.load() == 1);
    REQUIRE(begin_thread.load() == host.main_id());
    REQUIRE(store.get_value(7) == 0.9f);
    REQUIRE(store.gesture_thread_violation_count() == 0);
}

#ifdef NDEBUG
// Release-only: the debug assert in begin_gesture/end_gesture would abort this
// deliberate-misuse case. In release the assert compiles out and the violation
// counter is the observable enforcement signal. (Debug builds enforce via the
// assert instead, which is exercised by not being tripped in the tests above.)
TEST_CASE("off-main gesture with a live backend is counted (release)",
          "[state][gesture][thread]") {
    // A backend whose is_main_thread() is always false: the calling test thread
    // is treated as "not the main thread", simulating a background writer that
    // bypassed run_gesture_on_main and called begin_gesture directly.
    events::MainThreadDispatcher::Backend backend;
    backend.post = [](events::Task) { return true; };
    backend.is_main_thread = [] { return false; };
    auto token = events::MainThreadDispatcher::register_backend(std::move(backend));

    state::StateStore store;
    store.add_parameter({.id = 1, .name = "Gain", .range = {0.0f, 1.0f, 0.0f, 0.0f}});
    store.set_gesture_callbacks([](state::ParamID) {}, [](state::ParamID) {});

    REQUIRE(store.gesture_thread_violation_count() == 0);
    store.begin_gesture(1);
    store.end_gesture(1);
    REQUIRE(store.gesture_thread_violation_count() == 2);

    events::MainThreadDispatcher::unregister_backend(token);
}
#endif
