// MainThreadScope and MainThreadTimer inside a host's run loop (HostRunLoop),
// on platforms where the main thread is the host's. A fake host loop drives
// everything by hand: nothing runs until the test polls its descriptors or
// advances its clock, every callback runs on the test's own thread, and the
// platform layer starts no thread.

#include <pulp/platform/main_thread.hpp>

#include <catch2/catch_test_macros.hpp>

// Not built on Apple platforms, which run MainThreadScope on the main run loop.

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <dirent.h>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace pulp::platform;
using std::chrono::milliseconds;

namespace {

// A host's UI loop, driven by hand. Its "UI thread" is the thread that made it.
class FakeRunLoop final : public HostRunLoop {
  public:
    bool watch_fds = true;

    bool on_ui_thread() const override {
        return std::this_thread::get_id() == ui_;
    }

    std::uint64_t add_timer(milliseconds period, std::function<void()> tick) override {
        REQUIRE(on_ui_thread());
        const auto id = next_++;
        timers_[id] = {period, now_ + period, std::move(tick)};
        ++timers_added;
        return id;
    }
    void remove_timer(std::uint64_t id) override {
        REQUIRE(on_ui_thread());
        timers_.erase(id);
    }
    std::uint64_t add_fd(int fd, std::function<void()> readable) override {
        REQUIRE(on_ui_thread());
        if (!watch_fds)
            return 0;
        const auto id = next_++;
        fds_[id] = {fd, std::move(readable)};
        return id;
    }
    void remove_fd(std::uint64_t id) override {
        REQUIRE(on_ui_thread());
        fds_.erase(id);
    }

    // One pass of the host's loop over its descriptors.
    void poll_fds() {
        std::vector<std::uint64_t> ids;
        for (const auto& entry : fds_)
            ids.push_back(entry.first);
        for (auto id : ids) {
            auto it = fds_.find(id);
            if (it == fds_.end())
                continue;
            pollfd p{it->second.fd, POLLIN, 0};
            if (::poll(&p, 1, 0) > 0 && (p.revents & POLLIN)) {
                auto fn = it->second.fn;
                fn();
            }
        }
    }

    // Move the host's clock on and run every timer that is due.
    void advance(milliseconds by) {
        const auto end = now_ + by;
        for (;;) {
            std::uint64_t due = 0;
            milliseconds due_at = end + milliseconds(1);
            for (const auto& entry : timers_)
                if (entry.second.next <= end && entry.second.next < due_at) {
                    due = entry.first;
                    due_at = entry.second.next;
                }
            if (due == 0)
                break;
            now_ = due_at;
            auto& timer = timers_[due];
            timer.next += timer.period;
            auto tick = timer.tick;
            tick(); // may remove this timer
        }
        now_ = end;
    }

    std::size_t timers() const {
        return timers_.size();
    }
    std::size_t fds() const {
        return fds_.size();
    }
    int timers_added = 0;

  private:
    struct Timer {
        milliseconds period{0};
        milliseconds next{0};
        std::function<void()> tick;
    };
    struct Fd {
        int fd = -1;
        std::function<void()> fn;
    };
    std::thread::id ui_ = std::this_thread::get_id();
    std::map<std::uint64_t, Timer> timers_;
    std::map<std::uint64_t, Fd> fds_;
    std::uint64_t next_ = 1;
    milliseconds now_{0};
};

std::size_t open_descriptors() {
    std::size_t count = 0;
    if (DIR* dir = opendir("/proc/self/fd")) {
        while (readdir(dir) != nullptr)
            ++count;
        closedir(dir);
    }
    return count;
}

std::size_t thread_count() {
    std::size_t count = 0;
    if (DIR* dir = opendir("/proc/self/task")) {
        while (readdir(dir) != nullptr)
            ++count;
        closedir(dir);
    }
    return count;
}

// Registers a fake loop for the duration of a test.
struct Registered {
    FakeRunLoop loop;
    HostRunLoopToken token = register_host_run_loop(loop);
    ~Registered() {
        if (token != 0)
            unregister_host_run_loop(token);
    }
};

} // namespace

TEST_CASE("without a host run loop nothing is supported and posts are refused",
          "[platform][main-thread][host-run-loop]") {
    REQUIRE_FALSE(main_thread_supported());
    REQUIRE_FALSE(is_main_thread());
    MainThreadScope scope;
    bool ran = false;
    REQUIRE_FALSE(scope.post([&] { ran = true; }));
    // The scope exists, so a dialog opened under it finishes inline rather
    // than being dropped as if its owner were gone.
    REQUIRE(scope.handle().alive());
    REQUIRE_FALSE(scope.handle().post([&] { ran = true; }));
    MainThreadTimer timer(scope, [&] { ran = true; });
    timer.fire_after(milliseconds(1));
    timer.fire_every(milliseconds(1));
    REQUIRE_FALSE(timer.pending());
    REQUIRE_FALSE(timer.next_fire().has_value());
    REQUIRE_FALSE(ran);
}

TEST_CASE("a registered host loop makes the main thread supported",
          "[platform][main-thread][host-run-loop]") {
    REQUIRE_FALSE(main_thread_supported());
    {
        Registered host;
        REQUIRE(host.token != 0);
        REQUIRE(main_thread_supported());
        REQUIRE(is_main_thread());
        bool from_worker = true;
        std::thread worker([&] { from_worker = is_main_thread(); });
        worker.join();
        REQUIRE_FALSE(from_worker);
    }
    REQUIRE_FALSE(main_thread_supported());
    REQUIRE_FALSE(unregister_host_run_loop(12345));
}

TEST_CASE("a post from a worker runs on the host's UI thread when the host polls",
          "[platform][main-thread][host-run-loop]") {
    Registered host;
    MainThreadScope scope;
    REQUIRE(host.loop.fds() == 1);

    const auto ui = std::this_thread::get_id();
    std::vector<int> order;
    std::vector<bool> on_ui;
    std::thread worker([&] {
        for (int i = 1; i <= 3; ++i)
            scope.post([&, i] {
                order.push_back(i);
                on_ui.push_back(std::this_thread::get_id() == ui);
            });
    });
    worker.join();
    // Nothing runs until the host's loop does.
    REQUIRE(order.empty());
    host.loop.poll_fds();
    REQUIRE(order == std::vector<int>{1, 2, 3});
    REQUIRE(std::all_of(on_ui.begin(), on_ui.end(), [](bool b) { return b; }));
    // The wake byte was consumed: a second pass finds nothing.
    host.loop.poll_fds();
    REQUIRE(order.size() == 3);
}

TEST_CASE("a post made inside a task runs on a later pass",
          "[platform][main-thread][host-run-loop]") {
    Registered host;
    MainThreadScope scope;
    std::vector<int> order;
    scope.post([&] {
        order.push_back(1);
        scope.post([&] { order.push_back(3); });
        order.push_back(2);
    });
    host.loop.poll_fds();
    REQUIRE(order == std::vector<int>{1, 2});
    host.loop.poll_fds();
    REQUIRE(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("a task that throws is contained", "[platform][main-thread][host-run-loop]") {
    Registered host;
    MainThreadScope scope;
    bool after = false;
    scope.post([] { throw std::runtime_error("task"); });
    scope.post([&] { after = true; });
    host.loop.poll_fds();
    REQUIRE(after);
}

TEST_CASE("destroying a scope drops its queued tasks and removes its host registrations",
          "[platform][main-thread][host-run-loop]") {
    Registered host;
    int ran = 0;
    int teardowns = 0;
    MainThreadScope::Handle handle;
    {
        MainThreadScope scope;
        handle = scope.handle();
        REQUIRE(handle.alive());
        REQUIRE(host.loop.fds() == 1);
        scope.add_teardown([&] { ++teardowns; });
        const auto removed = scope.add_teardown([&] { ++teardowns; });
        scope.remove_teardown(removed);
        scope.post([&] { ++ran; });
    }
    REQUIRE(host.loop.fds() == 0);
    REQUIRE(host.loop.timers() == 0);
    REQUIRE(teardowns == 1);
    host.loop.poll_fds();
    REQUIRE(ran == 0);
    REQUIRE_FALSE(handle.alive());
    REQUIRE_FALSE(handle.post([&] { ++ran; }));
}

TEST_CASE("creating scopes and timers starts no thread and leaks no descriptor",
          "[platform][main-thread][host-run-loop]") {
    Registered host;
    {
        MainThreadScope warm;
    }
    const std::size_t threads = thread_count();
    const std::size_t fds = open_descriptors();
    for (int i = 0; i < 50; ++i) {
        MainThreadScope scope;
        MainThreadTimer timer(scope, [] {});
        timer.fire_every(milliseconds(5));
        scope.post([] {});
        host.loop.poll_fds();
        host.loop.advance(milliseconds(10));
    }
    REQUIRE(thread_count() == threads);
    REQUIRE(open_descriptors() == fds);
    REQUIRE(host.loop.timers() == 0);
    REQUIRE(host.loop.fds() == 0);
}

TEST_CASE("a repeating timer fires on the host's clock", "[platform][main-thread][host-run-loop]") {
    Registered host;
    MainThreadScope scope;
    int fires = 0;
    MainThreadTimer timer(scope, [&] { ++fires; });
    REQUIRE_FALSE(timer.pending());
    timer.fire_every(milliseconds(10));
    REQUIRE(timer.pending());
    REQUIRE(timer.next_fire().has_value());
    REQUIRE(timer.tolerance().count() == 0);
    host.loop.advance(milliseconds(9));
    REQUIRE(fires == 0);
    host.loop.advance(milliseconds(1));
    REQUIRE(fires == 1);
    host.loop.advance(milliseconds(30));
    REQUIRE(fires == 4);
    REQUIRE(timer.pending());

    timer.cancel();
    REQUIRE_FALSE(timer.pending());
    REQUIRE(host.loop.timers() == 0);
    host.loop.advance(milliseconds(100));
    REQUIRE(fires == 4);
}

TEST_CASE("a one-shot timer fires once and may re-arm itself",
          "[platform][main-thread][host-run-loop]") {
    Registered host;
    MainThreadScope scope;
    int fires = 0;
    MainThreadTimer* self = nullptr;
    MainThreadTimer timer(scope, [&] {
        ++fires;
        if (fires == 1)
            self->fire_after(milliseconds(5)); // re-arm from inside the callback
    });
    self = &timer;
    timer.fire_after(milliseconds(5));
    REQUIRE(timer.pending());
    host.loop.advance(milliseconds(5));
    REQUIRE(fires == 1);
    REQUIRE(timer.pending()); // re-armed
    host.loop.advance(milliseconds(5));
    REQUIRE(fires == 2);
    REQUIRE_FALSE(timer.pending());
    REQUIRE(host.loop.timers() == 0);
    host.loop.advance(milliseconds(100));
    REQUIRE(fires == 2);
}

TEST_CASE("fire_after replaces a pending fire", "[platform][main-thread][host-run-loop]") {
    Registered host;
    MainThreadScope scope;
    int fires = 0;
    MainThreadTimer timer(scope, [&] { ++fires; });
    timer.fire_after(milliseconds(50));
    timer.fire_after(milliseconds(5));
    REQUIRE(host.loop.timers() == 1);
    host.loop.advance(milliseconds(5));
    REQUIRE(fires == 1);
    host.loop.advance(milliseconds(100));
    REQUIRE(fires == 1);
}

TEST_CASE("a zero-delay timer fires on the next pass without a host timer",
          "[platform][main-thread][host-run-loop]") {
    Registered host;
    MainThreadScope scope;
    int fires = 0;
    MainThreadTimer timer(scope, [&] { ++fires; });
    timer.fire_after(milliseconds(0));
    REQUIRE(timer.pending());
    REQUIRE(host.loop.timers() == 0);
    REQUIRE(fires == 0);
    host.loop.poll_fds();
    REQUIRE(fires == 1);
    REQUIRE_FALSE(timer.pending());
    // Cancelled before the pass: it does not fire.
    timer.fire_after(milliseconds(0));
    timer.cancel();
    host.loop.poll_fds();
    REQUIRE(fires == 1);
}

TEST_CASE("destroying a timer, from inside its own callback too, stops it",
          "[platform][main-thread][host-run-loop]") {
    Registered host;
    MainThreadScope scope;
    int fires = 0;
    {
        MainThreadTimer timer(scope, [&] { ++fires; });
        timer.fire_every(milliseconds(5));
        REQUIRE(host.loop.timers() == 1);
    }
    REQUIRE(host.loop.timers() == 0);
    host.loop.advance(milliseconds(50));
    REQUIRE(fires == 0);

    std::unique_ptr<MainThreadTimer> timer;
    timer = std::make_unique<MainThreadTimer>(scope, [&] {
        ++fires;
        timer.reset();
    });
    timer->fire_every(milliseconds(5));
    host.loop.advance(milliseconds(50));
    REQUIRE(fires == 1);
    REQUIRE(host.loop.timers() == 0);
}

TEST_CASE("destroying the scope destroys the timers bound to it",
          "[platform][main-thread][host-run-loop]") {
    Registered host;
    int fires = 0;
    auto scope = std::make_unique<MainThreadScope>();
    MainThreadTimer timer(*scope, [&] { ++fires; });
    timer.fire_every(milliseconds(5));
    REQUIRE(host.loop.timers() == 1);
    scope.reset();
    REQUIRE(host.loop.timers() == 0);
    host.loop.advance(milliseconds(50));
    REQUIRE(fires == 0);
    REQUIRE_FALSE(timer.pending());
    // The timer is inert now; arming it again does nothing.
    timer.fire_every(milliseconds(5));
    REQUIRE_FALSE(timer.pending());
    REQUIRE(host.loop.timers() == 0);
}

TEST_CASE("unregistering the loop stops everything made under it first",
          "[platform][main-thread][host-run-loop]") {
    auto host = std::make_unique<Registered>();
    MainThreadScope scope;
    int ran = 0;
    int fires = 0;
    MainThreadTimer timer(scope, [&] { ++fires; });
    timer.fire_every(milliseconds(5));
    scope.post([&] { ++ran; });
    REQUIRE(host->loop.timers() == 1);
    REQUIRE(host->loop.fds() == 1);

    REQUIRE(unregister_host_run_loop(host->token));
    host->token = 0;
    // The host kept no callback into this layer.
    REQUIRE(host->loop.timers() == 0);
    REQUIRE(host->loop.fds() == 0);
    host->loop.poll_fds();
    host->loop.advance(milliseconds(50));
    REQUIRE(ran == 0);
    REQUIRE(fires == 0);
    // Later posts are refused, visibly.
    REQUIRE_FALSE(scope.post([&] { ++ran; }));
    REQUIRE(scope.handle().alive()); // it exists; it just takes no work
    REQUIRE_FALSE(timer.pending());
    REQUIRE_FALSE(main_thread_supported());
}

TEST_CASE("registrations nest: the latest is used and the previous returns",
          "[platform][main-thread][host-run-loop]") {
    Registered outer;
    MainThreadScope under_outer;
    {
        Registered inner;
        MainThreadScope under_inner;
        REQUIRE(inner.loop.fds() == 1);
        REQUIRE(outer.loop.fds() == 1); // only under_outer's
        int ran = 0;
        under_inner.post([&] { ++ran; });
        inner.loop.poll_fds();
        REQUIRE(ran == 1);
    }
    REQUIRE(main_thread_supported());
    int ran = 0;
    under_outer.post([&] { ++ran; });
    outer.loop.poll_fds();
    REQUIRE(ran == 1);
    MainThreadScope again;
    REQUIRE(outer.loop.fds() == 2);
}

TEST_CASE("a host that cannot watch descriptors is polled on its own timer",
          "[platform][main-thread][host-run-loop]") {
    Registered host;
    host.loop.watch_fds = false;
    MainThreadScope scope;
    REQUIRE(host.loop.fds() == 0);
    REQUIRE(host.loop.timers() == 1);
    int ran = 0;
    std::thread worker([&] { scope.post([&] { ++ran; }); });
    worker.join();
    REQUIRE(ran == 0);
    host.loop.advance(milliseconds(10));
    REQUIRE(ran == 1);
}

TEST_CASE("an editor can watch its own descriptor through the host loop",
          "[platform][main-thread][host-run-loop]") {
    Registered host;
    MainThreadScope scope;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    const std::size_t baseline = host.loop.fds(); // the scope's own wake pipe
    int calls = 0;
    {
        auto watch = scope.watch_fd(fds[0], [&] {
            ++calls;
            char sink[8];
            (void)!::read(fds[0], sink, sizeof sink);
        });
        REQUIRE(watch);
        REQUIRE(host.loop.fds() == baseline + 1);
        host.loop.poll_fds();
        REQUIRE(calls == 0); // nothing readable yet
        (void)!::write(fds[1], "x", 1);
        host.loop.poll_fds();
        REQUIRE(calls == 1);
        // Stopping the watch removes the host's registration.
        watch.reset();
        REQUIRE_FALSE(watch);
        REQUIRE(host.loop.fds() == baseline);
        (void)!::write(fds[1], "x", 1);
        host.loop.poll_fds();
        REQUIRE(calls == 1);
    }

    // A callback may stop its own watch.
    MainThreadScope::FdWatch self;
    int self_calls = 0;
    self = scope.watch_fd(fds[0], [&] {
        ++self_calls;
        char sink[8];
        (void)!::read(fds[0], sink, sizeof sink);
        self.reset();
    });
    (void)!::write(fds[1], "x", 1);
    host.loop.poll_fds();
    REQUIRE(self_calls == 1);
    REQUIRE(host.loop.fds() == baseline);

    // Destroying the scope with a watch live removes it; the handle is inert.
    auto scope2 = std::make_unique<MainThreadScope>();
    auto outlived = scope2->watch_fd(fds[0], [] {});
    REQUIRE(outlived);
    scope2.reset();
    REQUIRE(host.loop.fds() == baseline);
    outlived.reset(); // no effect, no crash

    // A host that cannot watch descriptors, or a refused descriptor, gives an
    // empty handle.
    REQUIRE_FALSE(scope.watch_fd(-1, [] {}));
    host.loop.watch_fds = false;
    REQUIRE_FALSE(scope.watch_fd(fds[0], [] {}));
    host.loop.watch_fds = true;
    ::close(fds[0]);
    ::close(fds[1]);
}

TEST_CASE("withdrawing the host loop stops a descriptor watch",
          "[platform][main-thread][host-run-loop]") {
    auto host = std::make_unique<Registered>();
    MainThreadScope scope;
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    auto watch = scope.watch_fd(fds[0], [] {});
    REQUIRE(watch);
    REQUIRE(unregister_host_run_loop(host->token));
    host->token = 0;
    REQUIRE(host->loop.fds() == 0);
    watch.reset(); // inert
    REQUIRE_FALSE(scope.watch_fd(fds[0], [] {}));
    ::close(fds[0]);
    ::close(fds[1]);
}
