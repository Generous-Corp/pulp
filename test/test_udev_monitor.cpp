// Cross-platform device-hotplug monitor (libudev on Linux) — #3327 / L4.
// The pure action-classifier and the honest-fail availability/lifecycle probe
// run on every platform (off Linux the monitor is a documented no-op). The
// live add/remove path needs a real udev event, exercised on the tartci Linux
// VM via `modprobe snd-dummy` / `modprobe -r snd-dummy`.

#include <catch2/catch_test_macros.hpp>

#include <pulp/runtime/udev_monitor.hpp>

using namespace pulp::runtime;

TEST_CASE("classify_udev_action maps udev ACTION strings", "[runtime][hotplug][udev][issue-3327]") {
    REQUIRE(classify_udev_action("add") == UdevChange::added);
    REQUIRE(classify_udev_action("remove") == UdevChange::removed);
    // Everything else is a non-hotplug action we must not treat as add/remove.
    REQUIRE(classify_udev_action("change") == UdevChange::other);
    REQUIRE(classify_udev_action("bind") == UdevChange::other);
    REQUIRE(classify_udev_action("unbind") == UdevChange::other);
    REQUIRE(classify_udev_action("move") == UdevChange::other);
    REQUIRE(classify_udev_action("") == UdevChange::other);
    REQUIRE(classify_udev_action(nullptr) == UdevChange::other);
}

TEST_CASE("UdevMonitor honest-fails without libudev and never crashes",
          "[runtime][hotplug][udev][issue-3327]") {
    // Off Linux (or without libudev) library_available() is false and start()
    // returns false — callers get no hotplug, no crash. On Linux+libudev+udevd
    // start() succeeds and the lifecycle is clean.
    const bool avail = UdevMonitor::library_available();

    UdevMonitor mon;
    REQUIRE_FALSE(mon.running());
    const bool started = mon.start({"sound"}, [](UdevChange) {});
    // start() can only succeed when libudev loaded (it may still fail with
    // libudev present — e.g. no udevd / restricted netlink — which is fine).
    if (started) {
        REQUIRE(avail);
        REQUIRE(mon.running());
        mon.stop();
    } else {
        // No monitor started → nothing running.
        REQUIRE_FALSE(mon.running());
    }
    REQUIRE_FALSE(mon.running());

    // stop() is idempotent and safe when never started.
    UdevMonitor unused;
    unused.stop();
    REQUIRE_FALSE(unused.running());
}
