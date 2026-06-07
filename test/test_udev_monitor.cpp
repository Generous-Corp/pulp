// Linux device-hotplug (libudev) monitor — issue #3327 / L4 (hotplug half).
// The pure action-classifier and the honest-fail availability probe are unit
// tested here; the live add/remove path needs a real udev event (exercised on
// the tartci Linux VM via `modprobe snd-dummy` / `modprobe -r snd-dummy`).
// Linux-only: the symbols live in pulp-audio only on Linux.

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__)

#include "../core/audio/platform/linux/udev_monitor.hpp"

using namespace pulp::audio::linux_platform;

TEST_CASE("classify_udev_action maps udev ACTION strings", "[audio][hotplug][udev][issue-3327]") {
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
          "[audio][hotplug][udev][issue-3327]") {
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
    }
    REQUIRE_FALSE(mon.running());

    // stop() is idempotent and safe when never started.
    UdevMonitor unused;
    unused.stop();
    REQUIRE_FALSE(unused.running());
}

#else  // !linux

TEST_CASE("udev hotplug monitor is Linux-only", "[audio][hotplug][udev][skip]") {
    SUCCEED("libudev device hotplug is Linux-only");
}

#endif
