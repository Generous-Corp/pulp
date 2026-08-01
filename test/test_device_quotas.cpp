#include <pulp/format/device_quotas.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

using pulp::format::DeviceQuotas;
using pulp::format::PreviewQuality;
using pulp::format::device_quotas;
using pulp::platform::DeviceCapabilityInputs;
using pulp::platform::DeviceCapabilityTier;
using pulp::platform::PointerPrecision;
using pulp::platform::ThermalState;
using pulp::platform::project_device_capability_tier;

namespace {

constexpr std::uint64_t kGiB = 1024ull * 1024 * 1024;

constexpr std::array<DeviceCapabilityTier, 3> kAllTiers = {
    DeviceCapabilityTier::Constrained,
    DeviceCapabilityTier::Standard,
    DeviceCapabilityTier::Full,
};

constexpr std::array<ThermalState, 4> kAllThermalStates = {
    ThermalState::Nominal,
    ThermalState::Fair,
    ThermalState::Serious,
    ThermalState::Critical,
};

// Two lane-shaped input sets that project to the same rung by different routes:
// the browser tab is held down by having no realtime render path despite ample
// memory and cores, the phone by memory. They exist to show the table is keyed
// by the rung and not by the lane that observed it.
constexpr DeviceCapabilityInputs kBrowserNoRealtime{
    .usable_memory_bytes = 16 * kGiB,
    .usable_core_count = 16,
    .thermal_reporting_available = false,
    .realtime_render_available = false,
    .pointer_precision = PointerPrecision::Fine,
    .precision_keyboard_available = true};

constexpr DeviceCapabilityInputs kPhone{
    .usable_memory_bytes = 2 * kGiB,
    .usable_core_count = 6,
    .thermal_reporting_available = true,
    .realtime_render_available = true,
    .pointer_precision = PointerPrecision::Coarse,
    .precision_keyboard_available = false};

} // namespace

TEST_CASE("thermal state transition only lowers quotas monotonically",
          "[format][device-quotas]") {
    for (const DeviceCapabilityTier tier : kAllTiers) {
        INFO("tier index: " << static_cast<int>(tier));

        // Every adjacent pair on the thermal ladder, not a sampled one.
        for (std::size_t index = 0; index + 1 < kAllThermalStates.size(); ++index) {
            const ThermalState cooler = kAllThermalStates[index];
            const ThermalState hotter = kAllThermalStates[index + 1];
            INFO("thermal transition: " << static_cast<int>(cooler) << " -> "
                                        << static_cast<int>(hotter));

            const DeviceQuotas cool = device_quotas(tier, cooler);
            const DeviceQuotas hot = device_quotas(tier, hotter);

            REQUIRE(hot.max_voices <= cool.max_voices);
            REQUIRE(hot.max_nodes <= cool.max_nodes);
            REQUIRE(hot.max_simultaneous_editors <= cool.max_simultaneous_editors);
            REQUIRE(hot.preview_quality <= cool.preview_quality);
        }

        // A table that never moved would satisfy the property above without
        // saying anything. Assert the table actually descends within each tier
        // so the monotonicity check has something to be true about.
        const DeviceQuotas nominal = device_quotas(tier, ThermalState::Nominal);
        const DeviceQuotas critical = device_quotas(tier, ThermalState::Critical);
        REQUIRE(critical.max_voices < nominal.max_voices);
        REQUIRE(critical.max_nodes < nominal.max_nodes);
        REQUIRE(critical.preview_quality < nominal.preview_quality);
    }
}

TEST_CASE("rising tier never lowers a quota at a fixed thermal state",
          "[format][device-quotas]") {
    for (const ThermalState thermal : kAllThermalStates) {
        INFO("thermal state: " << static_cast<int>(thermal));
        for (std::size_t index = 0; index + 1 < kAllTiers.size(); ++index) {
            const DeviceQuotas lower = device_quotas(kAllTiers[index], thermal);
            const DeviceQuotas higher = device_quotas(kAllTiers[index + 1], thermal);
            INFO("tier transition: " << static_cast<int>(kAllTiers[index]) << " -> "
                                     << static_cast<int>(kAllTiers[index + 1]));

            REQUIRE(higher.max_voices >= lower.max_voices);
            REQUIRE(higher.max_nodes >= lower.max_nodes);
            REQUIRE(higher.max_simultaneous_editors >= lower.max_simultaneous_editors);
            REQUIRE(higher.preview_quality >= lower.preview_quality);
        }
    }
}

TEST_CASE("the quota row is keyed by the rung, not by the lane that observed it",
          "[format][device-quotas]") {
    // A browser tab with sixteen cores and no realtime render path, and a phone
    // held down by memory, reach the same rung by different routes. The table
    // has to answer identically for both, or the tier ladder would have forked
    // into a per-lane one at the point of use.
    const DeviceCapabilityTier browser = project_device_capability_tier(kBrowserNoRealtime);
    const DeviceCapabilityTier phone = project_device_capability_tier(kPhone);
    REQUIRE(browser == phone);

    for (const ThermalState thermal : kAllThermalStates) {
        INFO("thermal state: " << static_cast<int>(thermal));
        REQUIRE(device_quotas(browser, thermal) == device_quotas(phone, thermal));
    }
}

TEST_CASE("an unknown tier or thermal value grants the least, not the most",
          "[format][device-quotas]") {
    // A value cast in from outside the declared enumerators must clamp rather
    // than read past the table. Assert the clamp lands on the restrictive end
    // of each axis specifically — a clamp to the wrong end would still be
    // in-bounds and still pass a bounds-only check.
    const auto unknown_tier = static_cast<DeviceCapabilityTier>(99);
    const auto unknown_thermal = static_cast<ThermalState>(99);

    REQUIRE(device_quotas(unknown_tier, ThermalState::Nominal) ==
            device_quotas(DeviceCapabilityTier::Constrained, ThermalState::Nominal));
    REQUIRE(device_quotas(DeviceCapabilityTier::Full, unknown_thermal) ==
            device_quotas(DeviceCapabilityTier::Full, ThermalState::Critical));
    REQUIRE(device_quotas(unknown_tier, unknown_thermal) ==
            device_quotas(DeviceCapabilityTier::Constrained, ThermalState::Critical));
}

TEST_CASE("quota lookup has no reachable mutable state", "[format][device-quotas]") {
    // Structural rather than observed, same argument as the tier projection:
    // constant evaluation is the proof. A lookup that touched any object whose
    // lifetime began outside the evaluation could not be constant evaluated at
    // all, so these compiling is a compile-time statement that reading a quota
    // has no reachable side effect.
    static_assert(std::is_trivially_copyable_v<DeviceQuotas>);
    static_assert(device_quotas(DeviceCapabilityTier::Full, ThermalState::Critical)
                      .preview_quality == PreviewQuality::Disabled);
    static_assert(device_quotas(DeviceCapabilityTier::Constrained, ThermalState::Nominal)
                      .max_simultaneous_editors == 1);

    SUCCEED("quota lookup has no reachable mutable state");
}
