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

TEST_CASE("ActionBroadcaster skips empty listener callbacks",
          "[events][async_updater][action_broadcaster][coverage][phase3]") {
    ActionBroadcaster broadcaster;
    std::vector<std::string> seen;

    const auto empty = broadcaster.add_listener({});
    broadcaster.add_listener([&](std::string_view action) {
        seen.emplace_back(action);
    });

    broadcaster.send_action("refresh");
    REQUIRE(seen == std::vector<std::string>{"refresh"});

    broadcaster.remove_listener(empty);
    broadcaster.send_action("again");
    REQUIRE(seen == std::vector<std::string>{"refresh", "again"});
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

TEST_CASE("ActionBroadcaster snapshots listeners during dispatch",
          "[events][async_updater][action_broadcaster][coverage]") {
    ActionBroadcaster broadcaster;
    std::vector<std::string> seen;
    int first_id = -1;

    first_id = broadcaster.add_listener([&](std::string_view action) {
        seen.emplace_back("first:" + std::string(action));
        broadcaster.remove_listener(first_id);
    });
    const auto second_id = broadcaster.add_listener([&](std::string_view action) {
        seen.emplace_back("second:" + std::string(action));
    });

    broadcaster.send_action("one");
    REQUIRE(seen == std::vector<std::string>{"first:one", "second:one"});

    broadcaster.send_action("two");
    REQUIRE(seen == std::vector<std::string>{
        "first:one", "second:one", "second:two"});

    broadcaster.remove_listener(second_id);
    broadcaster.send_action("ignored");
    REQUIRE(seen.size() == 3);
}

TEST_CASE("ActionBroadcaster delays listeners added during dispatch",
          "[events][async_updater][action_broadcaster][coverage][phase3]") {
    ActionBroadcaster broadcaster;
    std::vector<std::string> seen;
    int late_id = -1;

    const auto first_id = broadcaster.add_listener([&](std::string_view action) {
        seen.emplace_back("first:" + std::string(action));
        if (late_id < 0) {
            late_id = broadcaster.add_listener([&](std::string_view late_action) {
                seen.emplace_back("late:" + std::string(late_action));
            });
        }
    });
    broadcaster.add_listener([&](std::string_view action) {
        seen.emplace_back("second:" + std::string(action));
        broadcaster.remove_listener(first_id);
    });

    broadcaster.send_action("one");
    REQUIRE(seen == std::vector<std::string>{"first:one", "second:one"});

    broadcaster.send_action("two");
    REQUIRE(seen == std::vector<std::string>{
        "first:one", "second:one", "second:two", "late:two"});

    REQUIRE(late_id >= 0);
}

TEST_CASE("ActionBroadcaster tolerates null listeners",
          "[events][async_updater][action_broadcaster][coverage]") {
    ActionBroadcaster broadcaster;
    std::vector<std::string> seen;

    const auto null_id = broadcaster.add_listener({});
    const auto live_id = broadcaster.add_listener(
        [&](std::string_view action) { seen.emplace_back(action); });

    broadcaster.send_action("dispatch");
    REQUIRE(seen == std::vector<std::string>{"dispatch"});

    broadcaster.remove_listener(null_id);
    broadcaster.remove_listener(live_id);
    broadcaster.send_action("ignored");
    REQUIRE(seen.size() == 1);
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

TEST_CASE("MultiTimer supports zero and negative timer ids",
          "[events][async_updater][multi_timer][coverage]") {
    RecordingMultiTimer timers;

    timers.start_timer(0, 1);
    timers.start_timer(-4, 2);
    REQUIRE(timers.is_timer_running(0));
    REQUIRE(timers.is_timer_running(-4));
    REQUIRE_FALSE(timers.is_timer_running(4));

    timers.stop_timer(-4);
    REQUIRE(timers.is_timer_running(0));
    REQUIRE_FALSE(timers.is_timer_running(-4));
}

TEST_CASE("MultiTimer restart after stop_all works for existing entries",
          "[events][async_updater][multi_timer][coverage]") {
    RecordingMultiTimer timers;

    timers.start_timer(3, 10);
    timers.start_timer(4, 20);
    timers.stop_all_timers();

    timers.start_timer(4, 1);
    REQUIRE_FALSE(timers.is_timer_running(3));
    REQUIRE(timers.is_timer_running(4));

    timers.start_timer(3, 1);
    REQUIRE(timers.is_timer_running(3));
    REQUIRE(timers.is_timer_running(4));
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
