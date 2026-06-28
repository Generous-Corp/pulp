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

#include <atomic>
#include <chrono>
#include <thread>

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

// L5b smoke: open + start + stop a JACK device for real, but ONLY when a JACK
// server is actually reachable. In CI there is usually no jackd, so this skips
// cleanly (the build.yml Linux lane installs libjack so the TU still compiles +
// the enumeration contract above always runs — that is the regression guard for
// the L5a-class build break). On a JACK-enabled host (e.g. the tartci VM with
// `jackd -d dummy`) it proves the server drives our process callback.
TEST_CASE("JACK: open/start/stop round-trip when a server is reachable",
          "[audio][jack][issue-3327]") {
    if (!jack_is_available()) {
        SKIP("no JACK server reachable; open smoke cannot run");
        return;
    }

    JackSystem sys;
    auto dev = sys.create_device("jack");
    REQUIRE(dev != nullptr);

    DeviceConfig cfg;
    cfg.output_channels = 2;
    cfg.input_channels = 0;  // JACK supplies the real SR + buffer size
    REQUIRE(dev->open(cfg));
    REQUIRE(dev->is_open());
    CHECK(dev->sample_rate() > 0.0);
    CHECK(dev->buffer_size() > 0);

    std::atomic<int> callbacks{0};
    REQUIRE(dev->start([&](const BufferView<const float>&,
                           BufferView<float>& out,
                           const CallbackContext&) {
        for (std::size_t ch = 0; ch < out.num_channels(); ++ch)
            for (float& s : out.channel(ch)) s = 0.0f;  // silence
        callbacks.fetch_add(1, std::memory_order_relaxed);
    }));
    REQUIRE(dev->is_running());

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    dev->stop();
    dev->close();
    CHECK_FALSE(dev->is_running());
    CHECK(callbacks.load() > 0);  // the server delivered ≥1 process cycle
}

#else  // not (Linux && JACK)

TEST_CASE("JACK device enumeration is Linux+JACK only",
          "[audio][jack][skip]") {
    SUCCEED("JACK backend not compiled on this platform/configuration");
}

#endif
