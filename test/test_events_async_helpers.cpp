#include <catch2/catch_test_macros.hpp>
#include <pulp/events/async_updater.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace pulp::events;

namespace {

class ReentrantUpdater : public AsyncUpdater {
public:
    void handle_async_update() override {
        ++handles;
        if (handles == 1) {
            trigger_async_update();
        }
    }

    int handles = 0;
};

class CountingUpdater : public AsyncUpdater {
public:
    void handle_async_update() override {
        ++handles;
    }

    int handles = 0;
};

class RecordingMultiTimer : public MultiTimer {
public:
    void timer_callback(int timer_id) override {
        callbacks.push_back(timer_id);
    }

    std::vector<int> callbacks;
};

}  // namespace

TEST_CASE("AsyncUpdater keeps a reentrant trigger pending",
          "[events][async_updater][issue-642]") {
    ReentrantUpdater updater;

    updater.trigger_async_update();
    REQUIRE(updater.is_update_pending());

    updater.process_pending();
    REQUIRE(updater.handles == 1);
    REQUIRE(updater.is_update_pending());

    updater.process_pending();
    REQUIRE(updater.handles == 2);
    REQUIRE_FALSE(updater.is_update_pending());
}

TEST_CASE("LambdaAsyncUpdater clears pending state without a callback",
          "[events][async_updater][issue-642]") {
    LambdaAsyncUpdater updater({});

    updater.trigger_async_update();
    REQUIRE(updater.is_update_pending());

    updater.process_pending();
    REQUIRE_FALSE(updater.is_update_pending());
}

TEST_CASE("AsyncUpdater cancel and coalesce paths are deterministic",
          "[events][async_updater][issue-642]") {
    CountingUpdater updater;

    updater.process_pending();
    REQUIRE(updater.handles == 0);
    REQUIRE_FALSE(updater.is_update_pending());

    updater.trigger_async_update();
    updater.trigger_async_update();
    REQUIRE(updater.is_update_pending());

    updater.process_pending();
    REQUIRE(updater.handles == 1);
    REQUIRE_FALSE(updater.is_update_pending());

    updater.trigger_async_update();
    REQUIRE(updater.is_update_pending());
    updater.cancel_pending_update();
    REQUIRE_FALSE(updater.is_update_pending());
    updater.process_pending();
    REQUIRE(updater.handles == 1);

    updater.trigger_async_update();
    updater.process_pending();
    REQUIRE(updater.handles == 2);
}

TEST_CASE("LambdaAsyncUpdater invokes callbacks only for pending work",
          "[events][async_updater][issue-642]") {
    int calls = 0;
    LambdaAsyncUpdater updater([&] { ++calls; });

    updater.process_pending();
    REQUIRE(calls == 0);

    updater.trigger_async_update();
    updater.cancel_pending_update();
    updater.process_pending();
    REQUIRE(calls == 0);

    updater.trigger_async_update();
    updater.process_pending();
    REQUIRE(calls == 1);
    REQUIRE_FALSE(updater.is_update_pending());
}

TEST_CASE("ActionBroadcaster ignores missing listener removals",
          "[events][async_updater][action_broadcaster][issue-642]") {
    ActionBroadcaster broadcaster;
    std::vector<std::string> seen;

    const auto first = broadcaster.add_listener(
        [&](std::string_view action) { seen.emplace_back(action); });
    const auto second = broadcaster.add_listener(
        [&](std::string_view action) { seen.emplace_back("second:" + std::string(action)); });

    broadcaster.remove_listener(999);
    broadcaster.send_action("refresh");
    REQUIRE(seen == std::vector<std::string>{"refresh", "second:refresh"});

    broadcaster.remove_listener(first);
    broadcaster.remove_listener(first);
    broadcaster.send_action("rebuild");
    REQUIRE(seen == std::vector<std::string>{
        "refresh", "second:refresh", "second:rebuild"});

    broadcaster.remove_listener(second);
    broadcaster.send_action("ignored");
    REQUIRE(seen.size() == 3);
}

TEST_CASE("ActionBroadcaster handles empty actions and no-listener sends",
          "[events][async_updater][action_broadcaster][issue-642]") {
    ActionBroadcaster broadcaster;

    broadcaster.send_action("");
    broadcaster.remove_listener(123);

    std::vector<std::string> seen;
    auto listener = broadcaster.add_listener(
        [&](std::string_view action) { seen.emplace_back(action); });

    broadcaster.send_action("");
    broadcaster.send_action("named");
    REQUIRE(seen == std::vector<std::string>{"", "named"});

    broadcaster.remove_listener(listener);
    broadcaster.send_action("ignored");
    REQUIRE(seen.size() == 2);
}

TEST_CASE("ActionBroadcaster tolerates listener mutation during dispatch",
          "[events][async_updater][action_broadcaster][coverage][phase3]") {
    ActionBroadcaster broadcaster;
    std::vector<std::string> seen;

    int second = -1;
    const auto first = broadcaster.add_listener([&](std::string_view action) {
        seen.emplace_back("first:" + std::string(action));
        broadcaster.remove_listener(second);
        broadcaster.add_listener([&](std::string_view later) {
            seen.emplace_back("third:" + std::string(later));
        });
    });
    second = broadcaster.add_listener([&](std::string_view action) {
        seen.emplace_back("second:" + std::string(action));
        broadcaster.remove_listener(first);
    });

    broadcaster.send_action("initial");
    REQUIRE(seen == std::vector<std::string>{
        "first:initial", "second:initial"});

    broadcaster.send_action("later");
    REQUIRE(seen == std::vector<std::string>{
        "first:initial", "second:initial", "third:later"});
}

TEST_CASE("MultiTimer stop all permits selective restart",
          "[events][async_updater][multi_timer][issue-642]") {
    RecordingMultiTimer timers;

    timers.start_timer(1, 20);
    timers.start_timer(2, 10);
    timers.stop_all_timers();
    REQUIRE_FALSE(timers.is_timer_running(1));
    REQUIRE_FALSE(timers.is_timer_running(2));

    timers.start_timer(1, 5);
    REQUIRE(timers.is_timer_running(1));
    REQUIRE_FALSE(timers.is_timer_running(2));

    timers.stop_timer(1);
    REQUIRE_FALSE(timers.is_timer_running(1));
    REQUIRE_FALSE(timers.is_timer_running(99));

    timers.stop_timer(42);
    REQUIRE_FALSE(timers.is_timer_running(1));
    REQUIRE_FALSE(timers.is_timer_running(2));
}

TEST_CASE("MultiTimer restart keeps one running entry per id",
          "[events][async_updater][multi_timer][issue-642]") {
    RecordingMultiTimer timers;

    timers.start_timer(7, 20);
    timers.start_timer(7, 10);
    timers.start_timer(7, 5);
    REQUIRE(timers.is_timer_running(7));

    timers.stop_timer(7);
    REQUIRE_FALSE(timers.is_timer_running(7));

    timers.start_timer(7, 1);
    REQUIRE(timers.is_timer_running(7));

    timers.stop_all_timers();
    timers.stop_all_timers();
    REQUIRE_FALSE(timers.is_timer_running(7));
}

TEST_CASE("ScopedLowPowerModeDisabler has no observable test-lane side effects",
          "[events][async_updater][power][issue-642]") {
    ScopedLowPowerModeDisabler first;
    {
        ScopedLowPowerModeDisabler nested;
        SUCCEED("nested low-power disabler constructed");
    }
    SUCCEED("low-power disabler destructed");
}
