#include <pulp/platform/device_capability.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <string_view>
#include <type_traits>

using pulp::platform::DeviceCapabilityInputs;
using pulp::platform::DeviceCapabilityTier;
using pulp::platform::PointerPrecision;
using pulp::platform::project_device_capability_tier;

namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;

/// One synthetic device, named so a failure says which shape broke.
struct CapabilityCase {
    std::string_view name;
    DeviceCapabilityInputs inputs;
    DeviceCapabilityTier expected;
};

// Synthetic devices spanning both lanes the ladder has to absorb. The browser
// shapes differ from the mobile shapes only in the numbers a shell would
// observe, never in which enum they project onto. Each is named so the ladder
// test can reference it by meaning rather than by table position.

constexpr DeviceCapabilityInputs kBrowserNoRealtime{
    .usable_memory_bytes = 16 * kGiB,
    .usable_core_count = 16,
    .thermal_reporting_available = false,
    .realtime_render_available = false,
    .pointer_precision = PointerPrecision::Fine,
    .precision_keyboard_available = true};

constexpr DeviceCapabilityInputs kBrowserRealtime{
    .usable_memory_bytes = 16 * kGiB,
    .usable_core_count = 16,
    .thermal_reporting_available = false,
    .realtime_render_available = true,
    .pointer_precision = PointerPrecision::Fine,
    .precision_keyboard_available = true};

constexpr DeviceCapabilityInputs kBrowserMemoryStarved{
    .usable_memory_bytes = 1 * kGiB,
    .usable_core_count = 8,
    .thermal_reporting_available = true,
    .realtime_render_available = true,
    .pointer_precision = PointerPrecision::Fine,
    .precision_keyboard_available = true};

constexpr DeviceCapabilityInputs kPhone{
    .usable_memory_bytes = 2 * kGiB,
    .usable_core_count = 6,
    .thermal_reporting_available = true,
    .realtime_render_available = true,
    .pointer_precision = PointerPrecision::Coarse,
    .precision_keyboard_available = false};

constexpr DeviceCapabilityInputs kTabletTouchOnly{
    .usable_memory_bytes = 8 * kGiB,
    .usable_core_count = 8,
    .thermal_reporting_available = true,
    .realtime_render_available = true,
    .pointer_precision = PointerPrecision::Coarse,
    .precision_keyboard_available = false};

constexpr DeviceCapabilityInputs kTabletWithKeyboard{
    .usable_memory_bytes = 8 * kGiB,
    .usable_core_count = 8,
    .thermal_reporting_available = true,
    .realtime_render_available = true,
    .pointer_precision = PointerPrecision::Fine,
    .precision_keyboard_available = true};

constexpr DeviceCapabilityInputs kDesktop{
    .usable_memory_bytes = 32 * kGiB,
    .usable_core_count = 12,
    .thermal_reporting_available = true,
    .realtime_render_available = true,
    .pointer_precision = PointerPrecision::Fine,
    .precision_keyboard_available = true};

constexpr DeviceCapabilityInputs kDesktopFewCores{
    .usable_memory_bytes = 32 * kGiB,
    .usable_core_count = 4,
    .thermal_reporting_available = true,
    .realtime_render_available = true,
    .pointer_precision = PointerPrecision::Fine,
    .precision_keyboard_available = true};

constexpr DeviceCapabilityInputs kNothingObserved{};

constexpr std::array<CapabilityCase, 9> kCapabilityCases = {{
    {"browser tab without a realtime render path", kBrowserNoRealtime,
     DeviceCapabilityTier::Constrained},
    {"browser tab with a realtime render path but no thermal reporting", kBrowserRealtime,
     DeviceCapabilityTier::Standard},
    {"browser tab on a memory-starved device", kBrowserMemoryStarved,
     DeviceCapabilityTier::Constrained},
    {"phone", kPhone, DeviceCapabilityTier::Constrained},
    {"tablet on touch alone", kTabletTouchOnly, DeviceCapabilityTier::Standard},
    {"tablet with a keyboard and a trackpad", kTabletWithKeyboard,
     DeviceCapabilityTier::Full},
    {"desktop", kDesktop, DeviceCapabilityTier::Full},
    {"desktop with too few cores for the top rung", kDesktopFewCores,
     DeviceCapabilityTier::Standard},
    {"device that observed nothing", kNothingObserved,
     DeviceCapabilityTier::Constrained},
}};

} // namespace

TEST_CASE("tier computation is deterministic for fixed capability inputs",
          "[platform][device-capability]") {
    for (const CapabilityCase& scenario : kCapabilityCases) {
        INFO("device: " << scenario.name);
        REQUIRE(project_device_capability_tier(scenario.inputs) == scenario.expected);

        // Determinism is repetition, not just correctness: the same inputs
        // must project the same way every time, and a copy of the inputs must
        // project the same way as the original.
        const DeviceCapabilityInputs copy = scenario.inputs;
        for (int repeat = 0; repeat < 8; ++repeat) {
            REQUIRE(project_device_capability_tier(scenario.inputs) == scenario.expected);
            REQUIRE(project_device_capability_tier(copy) == scenario.expected);
        }
    }
}

TEST_CASE("browser and mobile capability inputs land on one tier ladder",
          "[platform][device-capability]") {
    // The browser lane's Tier A/B/C and the mobile lane's M-A/M-B/M-C are the
    // same three rungs. If a second ladder ever appeared, one of these pairs
    // would stop being comparable.
    const DeviceCapabilityTier browser_low =
        project_device_capability_tier(kBrowserNoRealtime);
    const DeviceCapabilityTier mobile_low = project_device_capability_tier(kPhone);
    const DeviceCapabilityTier mobile_mid = project_device_capability_tier(kTabletTouchOnly);
    const DeviceCapabilityTier mobile_high =
        project_device_capability_tier(kTabletWithKeyboard);

    REQUIRE(browser_low == mobile_low);
    REQUIRE(mobile_low < mobile_mid);
    REQUIRE(mobile_mid < mobile_high);
}

TEST_CASE("document is never mutated by tier projection",
          "[platform][device-capability]") {
    // This one is structural rather than observed. `device_capability.hpp`
    // includes no Pulp header, so no document, session, or transport type is
    // nameable from inside the projection; the inputs arrive by value and are
    // trivially copyable; and every entry point is constexpr.
    //
    // Constant evaluation is the proof: a function that touched any object
    // whose lifetime began outside the evaluation could not be constant
    // evaluated at all, so these static_asserts compiling is a compile-time
    // statement that the projection has no reachable side effect. A runtime
    // assertion could only ever check the documents this test happened to
    // build.
    static_assert(std::is_trivially_copyable_v<DeviceCapabilityInputs>);

    static_assert(project_device_capability_tier({}) == DeviceCapabilityTier::Constrained);
    static_assert(project_device_capability_tier(
                      {.usable_memory_bytes = 32 * kGiB,
                       .usable_core_count = 12,
                       .thermal_reporting_available = true,
                       .realtime_render_available = true,
                       .pointer_precision = PointerPrecision::Fine,
                       .precision_keyboard_available = true}) ==
                  DeviceCapabilityTier::Full);

    SUCCEED("tier projection has no reachable mutable state");
}
