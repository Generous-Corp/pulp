// Tests for the optional GPU NAM licensing gate. The gate is free-tier (unlocked)
// by default so the shipped MIT demo is unrestricted; once configured with a key
// it must lock on an invalid/absent license. These exercise the LicenseGate logic
// directly (the processor consults unlocked() to decide GPU eligibility).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>

#include "gpu_nam_license.hpp"

using pulp::examples::LicenseGate;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("License gate is free-tier and unlocked by default", "[nam][license]") {
    LicenseGate gate;
    CHECK(gate.unlocked());
    CHECK_THAT(gate.status_text(), ContainsSubstring("Free"));
}

TEST_CASE("Empty configuration stays free-tier", "[nam][license]") {
    LicenseGate gate;
    gate.configure_rsa("", "anything");   // no public key -> free tier
    CHECK(gate.unlocked());

    std::array<std::uint8_t, 32> secret{};
    gate.configure_shared(nullptr, 0, "anything");  // no secret -> free tier
    CHECK(gate.unlocked());
}

TEST_CASE("A configured gate locks on an invalid license", "[nam][license]") {
    // A real secret plus a garbage license key must not validate -> the gate locks
    // (the processor would then hold the GPU path to the CPU engine).
    std::array<std::uint8_t, 32> secret{};
    for (std::size_t i = 0; i < secret.size(); ++i) secret[i] = static_cast<std::uint8_t>(i + 1);

    LicenseGate gate;
    gate.configure_shared(secret.data(), secret.size(), "not-a-real-license-key");
    CHECK_FALSE(gate.unlocked());
    CHECK(gate.status() != pulp::runtime::LicenseStatus::Valid);
    CHECK_THAT(gate.status_text(), ContainsSubstring("Unregistered"));
}

TEST_CASE("A configured gate locks on an absent license", "[nam][license]") {
    std::array<std::uint8_t, 32> secret{};
    secret[0] = 0xAB;
    LicenseGate gate;
    gate.configure_shared(secret.data(), secret.size(), "");
    CHECK_FALSE(gate.unlocked());
}
