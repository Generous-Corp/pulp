// Verifies AudioSystem's base-class hotplug plumbing (workstream 02 slice 2.7).
// A non-macOS backend can now store a callback via the default
// set_device_change_callback() and fire it via fire_device_change()
// without having to reimplement the storage in each subclass.

#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/device.hpp>

#include <atomic>

using namespace pulp::audio;

namespace {

// Minimal AudioSystem that simulates a non-macOS backend using
// AudioSystem's base-class callback storage.
class StubSystem : public AudioSystem {
public:
    std::vector<DeviceInfo> enumerate_devices() override { return {}; }
    std::unique_ptr<AudioDevice> create_device(const std::string&) override {
        return nullptr;
    }
    DeviceInfo default_output_device() override { return {}; }
    DeviceInfo default_input_device() override { return {}; }
};

} // namespace

TEST_CASE("base class stores + fires the callback", "[audio][hotplug]") {
    StubSystem sys;
    std::atomic<int> calls{0};
    REQUIRE_FALSE(sys.has_device_change_callback());
    sys.set_device_change_callback([&] { ++calls; });
    REQUIRE(sys.has_device_change_callback());
    sys.fire_device_change();
    sys.fire_device_change();
    REQUIRE(calls.load() == 2);
}

TEST_CASE("registering a new callback replaces the previous callback", "[audio][hotplug]") {
    StubSystem sys;
    int first_calls = 0;
    int second_calls = 0;

    sys.set_device_change_callback([&] { ++first_calls; });
    sys.fire_device_change();
    sys.set_device_change_callback([&] { ++second_calls; });

    REQUIRE(sys.has_device_change_callback());
    sys.fire_device_change();
    REQUIRE(first_calls == 1);
    REQUIRE(second_calls == 1);
}

TEST_CASE("callback state persists across fires", "[audio][hotplug]") {
    StubSystem sys;
    int observed_calls = 0;

    sys.set_device_change_callback([calls = 0, &observed_calls]() mutable {
        observed_calls = ++calls;
    });

    sys.fire_device_change();
    REQUIRE(observed_calls == 1);
    sys.fire_device_change();
    REQUIRE(observed_calls == 2);
}

TEST_CASE("fire is a no-op with no callback", "[audio][hotplug]") {
    StubSystem sys;
    sys.fire_device_change();   // must not crash
    SUCCEED("fire_device_change returned cleanly with no callback");
}

TEST_CASE("nullptr unregisters the callback", "[audio][hotplug]") {
    StubSystem sys;
    int calls = 0;
    sys.set_device_change_callback([&] { ++calls; });
    sys.fire_device_change();
    sys.set_device_change_callback(nullptr);
    REQUIRE_FALSE(sys.has_device_change_callback());
    sys.fire_device_change();
    REQUIRE(calls == 1);
}

TEST_CASE("repeated unregister remains a no-op", "[audio][hotplug]") {
    StubSystem sys;
    int calls = 0;

    sys.set_device_change_callback(nullptr);
    REQUIRE_FALSE(sys.has_device_change_callback());
    sys.set_device_change_callback([&] { ++calls; });
    REQUIRE(sys.has_device_change_callback());
    sys.set_device_change_callback(nullptr);
    sys.set_device_change_callback(nullptr);

    REQUIRE_FALSE(sys.has_device_change_callback());
    sys.fire_device_change();
    REQUIRE(calls == 0);
}

TEST_CASE("callback may unregister itself during fire", "[audio][hotplug]") {
    StubSystem sys;
    int calls = 0;

    sys.set_device_change_callback([&] {
        ++calls;
        sys.set_device_change_callback(nullptr);
    });

    sys.fire_device_change();
    REQUIRE(calls == 1);
    REQUIRE_FALSE(sys.has_device_change_callback());

    sys.fire_device_change();
    REQUIRE(calls == 1);
}

TEST_CASE("callback may replace itself during fire", "[audio][hotplug]") {
    StubSystem sys;
    int first_calls = 0;
    int second_calls = 0;

    sys.set_device_change_callback([&] {
        ++first_calls;
        sys.set_device_change_callback([&] { ++second_calls; });
    });

    sys.fire_device_change();
    REQUIRE(first_calls == 1);
    REQUIRE(second_calls == 0);
    REQUIRE(sys.has_device_change_callback());

    sys.fire_device_change();
    REQUIRE(first_calls == 1);
    REQUIRE(second_calls == 1);
}
