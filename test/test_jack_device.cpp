// JACK device-enumeration contract (issue #3327 / Linux catch-up L5a).
//
// `JackSystem::enumerate_devices()` previously wrote `DeviceInfo` fields
// that don't exist on the struct (`default_sample_rate`, `is_default`),
// so the JACK translation unit failed to compile whenever CMake found a
// JACK dev package — a latent build break that only surfaced on a
// JACK-enabled Linux configure. This suite is the regression guard: it
// only compiles against the corrected field names (`sample_rates`,
// `is_default_input`/`is_default_output`), so its mere existence proves
// the TU builds, and the assertions pin the enumerated contract.
//
// Linux + JACK only. The target is registered solely when CMake detected
// JACK (PULP_JACK_AVAILABLE), so the real branch always applies there; the
// guards below keep the source honest if it is ever compiled elsewhere.

#include <catch2/catch_test_macros.hpp>

#if defined(__linux__) && defined(PULP_HAS_JACK)

#include <pulp/audio/device.hpp>
#include "../core/audio/platform/linux/jack_device.hpp"

using namespace pulp::audio;
using namespace pulp::audio::linux_platform;

TEST_CASE("JACK: enumerate_devices reports a coherent single server device",
          "[audio][jack][issue-3327]") {
    JackSystem sys;
    auto devices = sys.enumerate_devices();

    // JACK exposes exactly one logical "server" device.
    REQUIRE(devices.size() == 1);
    const DeviceInfo& info = devices.front();

    REQUIRE(info.id == "jack");
    REQUIRE_FALSE(info.name.empty());

    // The fields that previously did not exist must now be populated.
    REQUIRE_FALSE(info.sample_rates.empty());
    REQUIRE(info.sample_rates.front() > 0.0);
    REQUIRE_FALSE(info.buffer_sizes.empty());
    REQUIRE(info.is_default_input);
    REQUIRE(info.is_default_output);
}

TEST_CASE("JACK: advertised channel count matches the device open cap",
          "[audio][jack][issue-3327]") {
    // enumerate_devices() used to advertise 64 channels while the device
    // path (JackDevice::open) hard-caps registration at 8 ports — a host
    // could request more channels than the device would ever create. Both
    // sites now derive from one constant, so enumeration is honest.
    JackSystem sys;
    auto devices = sys.enumerate_devices();
    REQUIRE(devices.size() == 1);
    const DeviceInfo& info = devices.front();

    REQUIRE(info.max_input_channels == 8);
    REQUIRE(info.max_output_channels == 8);
}

TEST_CASE("JACK: default in/out devices mirror the enumerated server",
          "[audio][jack][issue-3327]") {
    JackSystem sys;
    auto enumerated = sys.enumerate_devices();
    REQUIRE(enumerated.size() == 1);

    DeviceInfo out = sys.default_output_device();
    DeviceInfo in = sys.default_input_device();

    REQUIRE(out.id == enumerated.front().id);
    REQUIRE(in.id == enumerated.front().id);
    REQUIRE(out.is_default_output);
    REQUIRE(in.is_default_input);
}

#else  // not (Linux && JACK)

TEST_CASE("JACK device enumeration is Linux+JACK only",
          "[audio][jack][skip]") {
    SUCCEED("JACK backend not compiled on this platform/configuration");
}

#endif
