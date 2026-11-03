// test_main_thread_timer.cpp — detail::MainThreadTimer, the cancellable
// main-thread timer behind contract editors' idle ticks: it ticks on the main
// thread, and once stopped (from the main thread or another one) it never
// ticks again.

#include <catch2/catch_test_macros.hpp>

#include <pulp/events/main_thread_dispatcher.hpp>
#include <pulp/format/detail/main_thread_timer.hpp>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#endif

#include <atomic>
#include <thread>
#include <vector>

using pulp::format::detail::MainThreadTimer;

namespace {

// The main thread's event loop: a dispatch source fires from it on Apple
// platforms; elsewhere the timer posts through MainThreadDispatcher, run here
// through a queued backend.
class MainLoop {
  public:
#if !defined(__APPLE__)
    MainLoop() {
        pulp::events::MainThreadDispatcher::Backend backend;
        backend.post = [this](pulp::events::Task task) {
            tasks_.push_back(std::move(task));
            return true;
        };
        backend.is_main_thread = [] { return true; };
        backend.post_after = [this](pulp::events::Task task, int) {
            tasks_.push_back(std::move(task));
            return true;
        };
        token_ = pulp::events::MainThreadDispatcher::register_backend(std::move(backend));
    }
    ~MainLoop() {
        pulp::events::MainThreadDispatcher::unregister_backend(token_);
    }
#endif
    void run_for(double seconds) {
#if defined(__APPLE__)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
#else
        for (int i = 0; i < static_cast<int>(seconds * 100.0); ++i) {
            auto batch = std::move(tasks_);
            tasks_.clear();
            for (auto& task : batch)
                task();
        }
#endif
    }
    template <typename Done>
    bool run_until(Done done, double seconds = 2.0) {
        for (double waited = 0.0; waited < seconds; waited += 0.02) {
            if (done())
                return true;
            run_for(0.02);
        }
        return done();
    }

  private:
#if !defined(__APPLE__)
    std::vector<pulp::events::Task> tasks_;
    pulp::events::MainThreadDispatcher::Token token_ = 0;
#endif
};

} // namespace

TEST_CASE("a main-thread timer ticks until it is stopped, then never again",
          "[editor-host][main-thread-timer]") {
    MainLoop loop;
    MainThreadTimer timer;
    int ticks = 0;
    REQUIRE(timer.start(5, [&ticks] { ++ticks; }));
    REQUIRE(timer.running());
    REQUIRE(loop.run_until([&ticks] { return ticks >= 3; }));

    timer.stop();
    REQUIRE_FALSE(timer.running());
    const int at_stop = ticks;
    loop.run_for(0.1);
    REQUIRE(ticks == at_stop);

    // Restarted, it ticks again.
    REQUIRE(timer.start(5, [&ticks] { ++ticks; }));
    REQUIRE(loop.run_until([&ticks, at_stop] { return ticks > at_stop; }));
}

TEST_CASE("a destroyed main-thread timer never ticks", "[editor-host][main-thread-timer]") {
    MainLoop loop;
    int ticks = 0;
    {
        MainThreadTimer timer;
        REQUIRE(timer.start(5, [&ticks] { ++ticks; }));
        REQUIRE(loop.run_until([&ticks] { return ticks >= 1; }));
    }
    const int at_destroy = ticks;
    loop.run_for(0.1);
    REQUIRE(ticks == at_destroy);
}

#if defined(__APPLE__)
TEST_CASE("a main-thread timer stopped from another thread never ticks again",
          "[editor-host][main-thread-timer]") {
    MainLoop loop;
    MainThreadTimer timer;
    std::atomic<int> ticks{0};
    REQUIRE(timer.start(5, [&ticks] { ++ticks; }));
    REQUIRE(loop.run_until([&ticks] { return ticks.load() >= 2; }));

    // stop() from a worker waits for the main thread to cancel the source, so
    // the main loop keeps running until the worker is done.
    std::atomic<bool> stopped{false};
    std::thread worker([&] {
        timer.stop();
        stopped = true;
    });
    REQUIRE(loop.run_until([&stopped] { return stopped.load(); }));
    worker.join();
    const int at_stop = ticks.load();
    loop.run_for(0.1);
    REQUIRE(ticks.load() == at_stop);
}
#endif
