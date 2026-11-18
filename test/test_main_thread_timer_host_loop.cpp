// detail::MainThreadTimer on a host run loop: stopping it from another thread
// after the loop was withdrawn must destroy nothing on that thread. The tick
// carries a sentinel whose destructor records where it ran.

#include <pulp/format/detail/main_thread_timer.hpp>
#include <pulp/platform/main_thread.hpp>

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <thread>

using namespace pulp::platform;

namespace {

class FakeLoop final : public HostRunLoop {
  public:
    bool on_ui_thread() const override {
        return std::this_thread::get_id() == ui_;
    }
    std::uint64_t add_timer(std::chrono::milliseconds, std::function<void()> tick) override {
        timers_[next_] = std::move(tick);
        return next_++;
    }
    void remove_timer(std::uint64_t id) override {
        timers_.erase(id);
    }
    std::uint64_t add_fd(int, std::function<void()>) override {
        return 1000 + fds_++;
    }
    void remove_fd(std::uint64_t) override {}
    std::size_t timers() const {
        return timers_.size();
    }

  private:
    std::thread::id ui_ = std::this_thread::get_id();
    std::map<std::uint64_t, std::function<void()>> timers_;
    std::uint64_t next_ = 1;
    std::uint64_t fds_ = 0;
};

// Records the thread that destroys the last copy.
struct Canary {
    struct State {
        explicit State(std::thread::id* w) : where(w) {}
        std::thread::id* where;
        ~State() {
            *where = std::this_thread::get_id();
        }
    };
    std::shared_ptr<State> state;
    void operator()() const {}
};

} // namespace

TEST_CASE("stopping a hosted timer off-thread after the loop is withdrawn destroys nothing there",
          "[main-thread][host-run-loop][lifetime]") {
    std::thread::id destroyed_on{};
    const auto ui = std::this_thread::get_id();
    FakeLoop loop;
    const auto token = register_host_run_loop(loop);
    pulp::format::detail::MainThreadTimer timer;
    Canary canary{std::make_shared<Canary::State>(&destroyed_on)};
    REQUIRE(timer.start(16, [canary] { canary(); }));
    canary.state.reset();
    REQUIRE(loop.timers() == 1);

    // The host's loop goes away first.
    REQUIRE(unregister_host_run_loop(token));
    REQUIRE(loop.timers() == 0);

    std::thread::id worker_id;
    std::thread worker([&] {
        worker_id = std::this_thread::get_id();
        timer.stop();
    });
    worker.join();
    CAPTURE(destroyed_on == worker_id, destroyed_on == ui);
    // Nothing was destroyed on the worker thread.
    REQUIRE(destroyed_on == std::thread::id{});

    // The next stop or start on the UI thread, once a host loop is registered
    // again, finishes the teardown there.
    FakeLoop next_loop;
    const auto next_token = register_host_run_loop(next_loop);
    timer.stop();
    REQUIRE(destroyed_on == ui);
    unregister_host_run_loop(next_token);
}
