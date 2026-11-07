#include <catch2/catch_test_macros.hpp>

#include "support/thread_progress.hpp"

#include <pulp/runtime/background_job.hpp>
#include <pulp/runtime/cancel.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace pulp::runtime;
using namespace std::chrono_literals;

TEST_CASE("CancelToken default is never cancelled", "[cancel]") {
    CancelToken token;
    CHECK_FALSE(token.is_cancelled());
    CHECK_FALSE(token.can_be_cancelled());
    CHECK_FALSE(token.wait_for_cancellation(1ms));
    bool ran = false;
    auto reg = token.register_callback([&] { ran = true; });
    CHECK_FALSE(reg);
    CHECK_FALSE(ran);
}

TEST_CASE("CancelSource cancel is idempotent and visible to copies", "[cancel]") {
    CancelSource source;
    CancelToken a = source.token();
    CancelToken b = a;
    CHECK(a.shares(b));
    CHECK_FALSE(a.is_cancelled());
    CHECK(source.cancel());
    CHECK_FALSE(source.cancel());
    CHECK(a.is_cancelled());
    CHECK(b.is_cancelled());
    CHECK(source.is_cancelled());
}

TEST_CASE("CancelSource destruction cancels outstanding tokens", "[cancel]") {
    CancelToken token;
    {
        CancelSource source;
        token = source.token();
        CHECK_FALSE(token.is_cancelled());
    }
    CHECK(token.is_cancelled());
}

TEST_CASE("CancelSource move transfers ownership without cancelling", "[cancel]") {
    CancelSource a;
    CancelToken token = a.token();
    CancelSource b = std::move(a);
    CHECK_FALSE(token.is_cancelled());
    a.cancel(); // moved-from is inert
    CHECK_FALSE(token.is_cancelled());
    b.cancel();
    CHECK(token.is_cancelled());
}

TEST_CASE("CancelSource move-assign cancels the replaced state", "[cancel]") {
    CancelSource a;
    CancelSource b;
    CancelToken ta = a.token();
    CancelToken tb = b.token();
    a = std::move(b);
    CHECK(ta.is_cancelled());
    CHECK_FALSE(tb.is_cancelled());
}

TEST_CASE("is_cancelled can be polled from a second thread", "[cancel]") {
    CancelSource source;
    CancelToken token = source.token();
    std::atomic<bool> seen{false};
    std::atomic<bool> started{false};
    std::thread poller([&] {
        started = true;
        pulp::test::wait_for_condition([&] { return token.is_cancelled(); });
        seen = true;
    });
    REQUIRE(pulp::test::wait_for_condition([&] { return started.load(); }));
    std::this_thread::sleep_for(5ms);
    CHECK_FALSE(seen);
    source.cancel();
    poller.join();
    CHECK(seen);
}

TEST_CASE("wait_for_cancellation wakes on cancel and times out otherwise", "[cancel]") {
    CancelSource source;
    CancelToken token = source.token();
    CHECK_FALSE(token.wait_for_cancellation(10ms));

    std::atomic<bool> result{false};
    std::thread waiter([&] { result = token.wait_for_cancellation(10s); });
    std::this_thread::sleep_for(10ms);
    source.cancel();
    waiter.join();
    CHECK(result);
    CHECK(token.wait_for_cancellation(0ms));
}

TEST_CASE("callbacks run on cancel, in order, once", "[cancel]") {
    CancelSource source;
    CancelToken token = source.token();
    std::vector<int> order;
    auto r1 = token.register_callback([&] { order.push_back(1); });
    auto r2 = token.register_callback([&] { order.push_back(2); });
    CHECK(r1);
    CHECK(r2);
    source.cancel();
    source.cancel();
    CHECK(order == std::vector<int>{1, 2});
}

TEST_CASE("callback registered after cancel runs immediately", "[cancel]") {
    CancelSource source;
    source.cancel();
    bool ran = false;
    auto reg = source.token().register_callback([&] { ran = true; });
    CHECK(ran);
    CHECK_FALSE(reg);
}

TEST_CASE("destroyed registration never runs", "[cancel]") {
    CancelSource source;
    bool ran = false;
    {
        auto reg = source.token().register_callback([&] { ran = true; });
        CHECK(reg);
    }
    source.cancel();
    CHECK_FALSE(ran);

    // reset() and moved-from registrations are inert too.
    CancelSource s2;
    bool ran2 = false;
    auto reg2 = s2.token().register_callback([&] { ran2 = true; });
    auto moved = std::move(reg2);
    CHECK_FALSE(reg2);
    moved.reset();
    s2.cancel();
    CHECK_FALSE(ran2);
}

TEST_CASE("callback may register another callback and see cancelled state", "[cancel]") {
    CancelSource source;
    CancelToken token = source.token();
    bool inner_ran = false;
    bool saw_cancelled = false;
    CancelRegistration inner;
    auto outer = token.register_callback([&] {
        saw_cancelled = token.is_cancelled();
        inner = token.register_callback([&] { inner_ran = true; });
    });
    source.cancel();
    CHECK(saw_cancelled);
    CHECK(inner_ran);
}

TEST_CASE("callback may destroy its own registration", "[cancel]") {
    CancelSource source;
    CancelRegistration reg;
    bool ran = false;
    reg = source.token().register_callback([&] {
        ran = true;
        reg.reset(); // must not deadlock waiting on itself
    });
    source.cancel();
    CHECK(ran);
}

TEST_CASE("registration destructor waits for a running callback", "[cancel]") {
    CancelSource source;
    std::atomic<bool> in_callback{false};
    std::atomic<bool> release{false};
    std::atomic<bool> finished{false};
    auto reg = std::make_unique<CancelRegistration>(source.token().register_callback([&] {
        in_callback = true;
        pulp::test::wait_for_condition([&] { return release.load(); });
        finished = true;
    }));
    std::thread canceller([&] { source.cancel(); });
    REQUIRE(pulp::test::wait_for_condition([&] { return in_callback.load(); }));

    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        reg.reset();
        destroyed = true;
    });
    std::this_thread::sleep_for(20ms);
    CHECK_FALSE(destroyed); // blocked while the callback runs
    release = true;
    destroyer.join();
    canceller.join();
    CHECK(finished);
    CHECK(destroyed);
}

TEST_CASE("cancelling from many threads runs each callback once", "[cancel]") {
    CancelSource source;
    std::atomic<int> count{0};
    auto reg = source.token().register_callback([&] { count++; });
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i)
        threads.emplace_back([&] { source.cancel(); });
    for (auto& t : threads)
        t.join();
    CHECK(count == 1);
}

TEST_CASE("CancellationToken keeps its shared request-and-observe behaviour", "[cancel]") {
    CancellationToken token;
    CancellationToken copy = token;
    CancellationToken other;
    CHECK(token.shares(copy));
    CHECK_FALSE(token.shares(other));
    CancelToken observer = copy; // converts to the observer type
    CHECK_FALSE(observer.is_cancelled());
    copy.cancel();
    CHECK(token.is_cancelled());
    CHECK(observer.is_cancelled());
    CHECK_FALSE(other.is_cancelled());
}

TEST_CASE("background job context observes handle cancellation", "[cancel]") {
    BackgroundJobService service;
    std::atomic<bool> started{false};
    std::atomic<bool> saw_cancel{false};
    auto handle = service.submit({"cancel-test"}, [&](BackgroundJobContext& ctx) {
        started = true;
        CHECK(ctx.cancellation_token().wait_for_cancellation(10s));
        saw_cancel = ctx.is_cancelled();
    });
    REQUIRE(pulp::test::wait_for_condition([&] { return started.load(); }));
    handle.cancel();
    CHECK(pulp::test::wait_for_condition([&] { return handle.is_finished(); }));
    CHECK(saw_cancel);
    CHECK(handle.is_cancelled());
}
