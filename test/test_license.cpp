#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/license.hpp>
#include <pulp/runtime/big_integer.hpp>
#include <pulp/runtime/base64.hpp>

using namespace pulp::runtime;

// ── BigInteger ──────────────────────────────────────────────────────────

TEST_CASE("BigInteger from uint64", "[crypto][bigint]") {
    BigInteger a(42);
    REQUIRE(a.to_string() == "42");
    REQUIRE_FALSE(a.is_zero());
}

TEST_CASE("BigInteger zero", "[crypto][bigint]") {
    BigInteger z(0);
    REQUIRE(z.is_zero());
    REQUIRE(z.to_string() == "0");
}

TEST_CASE("BigInteger from decimal string", "[crypto][bigint]") {
    auto a = BigInteger::from_string("123456789");
    REQUIRE(a.to_string() == "123456789");
}

TEST_CASE("BigInteger from hex", "[crypto][bigint]") {
    auto a = BigInteger::from_hex("FF");
    REQUIRE(a.to_string() == "255");
}

TEST_CASE("BigInteger addition", "[crypto][bigint]") {
    BigInteger a(100);
    BigInteger b(200);
    auto c = a + b;
    REQUIRE(c.to_string() == "300");
}

TEST_CASE("BigInteger multiplication", "[crypto][bigint]") {
    BigInteger a(12);
    BigInteger b(34);
    auto c = a * b;
    REQUIRE(c.to_string() == "408");
}

TEST_CASE("BigInteger modulo", "[crypto][bigint]") {
    BigInteger a(17);
    BigInteger b(5);
    auto c = a % b;
    REQUIRE(c.to_string() == "2");
}

TEST_CASE("BigInteger mod_pow", "[crypto][bigint]") {
    // 3^7 mod 13 = 2187 mod 13 = 3
    BigInteger base(3);
    BigInteger exp(7);
    BigInteger mod(13);
    auto result = base.mod_pow(exp, mod);
    REQUIRE(result.to_string() == "3");
}

TEST_CASE("BigInteger comparison", "[crypto][bigint]") {
    BigInteger a(100);
    BigInteger b(200);
    REQUIRE(a < b);
    REQUIRE(a != b);
    REQUIRE(a == BigInteger(100));
}

TEST_CASE("BigInteger large numbers", "[crypto][bigint]") {
    auto a = BigInteger::from_string("999999999999999999");
    auto b = BigInteger::from_string("1");
    auto c = a + b;
    REQUIRE(c.to_string() == "1000000000000000000");
}

// ── License ─────────────────────────────────────────────────────────────

TEST_CASE("LicenseValidator invalid format", "[crypto][license]") {
    LicenseValidator validator;
    REQUIRE(validator.validate("not-a-license") == LicenseStatus::InvalidFormat);
}

TEST_CASE("LicenseValidator missing dot separator", "[crypto][license]") {
    LicenseValidator validator;
    REQUIRE(validator.validate("nodot") == LicenseStatus::InvalidFormat);
}

TEST_CASE("LicenseValidator parse payload", "[crypto][license]") {
    LicenseValidator validator;

    // Create a fake license with valid base64 payload but no valid signature
    std::string payload = "{\"product_id\":\"PulpSynth\",\"email\":\"test@test.com\",\"issued\":1234567890}";
    std::string encoded = base64_encode(payload) + ".invalidsig";

    // Without a public key set, signature verification fails
    auto status = validator.validate(encoded);
    REQUIRE(status == LicenseStatus::InvalidSignature);
}

TEST_CASE("LicenseValidator validate_and_parse extracts info", "[crypto][license]") {
    LicenseValidator validator;

    std::string payload = "{\"product_id\":\"PulpGain\",\"email\":\"user@example.com\",\"edition\":\"pro\",\"issued\":1700000000}";
    std::string key = base64_encode(payload) + ".sig";

    auto info = validator.validate_and_parse(key);
    REQUIRE(info.has_value());
    REQUIRE(info->product_id == "PulpGain");
    REQUIRE(info->user_email == "user@example.com");
    REQUIRE(info->edition == "pro");
}

TEST_CASE("LicenseValidator is_valid_for_machine", "[crypto][license]") {
    LicenseValidator validator;

    LicenseInfo info;
    info.product_id = "test";

    // Empty machine_id = valid for any machine
    info.machine_id = "";
    REQUIRE(validator.is_valid_for_machine(info));

    // Matching machine_id
    info.machine_id = machine_id();
    REQUIRE(validator.is_valid_for_machine(info));

    // Non-matching
    info.machine_id = "wrong_machine";
    REQUIRE_FALSE(validator.is_valid_for_machine(info));
}

TEST_CASE("LicenseValidator file not found", "[crypto][license]") {
    LicenseValidator validator;
    REQUIRE(validator.validate_file("/tmp/nonexistent_license_12345.key") == LicenseStatus::NotFound);
}
