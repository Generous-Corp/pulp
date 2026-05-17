#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/license.hpp>
#include <pulp/runtime/big_integer.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/temporary_file.hpp>

#include <fstream>
#include <utility>

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

TEST_CASE("BigInteger invalid decimal and hex parse as zero",
          "[crypto][bigint][coverage][phase3]") {
    REQUIRE(BigInteger::from_string("not-a-number").is_zero());
    REQUIRE(BigInteger::from_hex("not-hex").is_zero());
    REQUIRE(BigInteger::from_string("").is_zero());
}

TEST_CASE("BigInteger copy move hex and bit-count helpers", "[crypto][bigint][coverage][issue-656]") {
    auto value = BigInteger::from_hex("0100");
    REQUIRE(value.to_string() == "256");
    REQUIRE(value.to_hex() == "0100");
    REQUIRE(value.bit_count() == 9);

    BigInteger copied(value);
    REQUIRE(copied == value);

    BigInteger assigned;
    assigned = copied;
    REQUIRE(assigned == value);

    BigInteger moved(std::move(assigned));
    REQUIRE(moved == value);

    BigInteger move_assigned;
    move_assigned = std::move(moved);
    REQUIRE(move_assigned.to_string() == "256");
}

TEST_CASE("BigInteger hex bit count and copy assignment",
          "[crypto][bigint][coverage]") {
    auto value = BigInteger::from_hex("0100");
    REQUIRE(value.to_hex() == "0100");
    REQUIRE(value.bit_count() == 9);

    BigInteger copy;
    copy = value;
    REQUIRE(copy == value);
    REQUIRE(copy.to_string() == "256");
}

TEST_CASE("BigInteger move construction and move assignment leave values usable",
          "[crypto][bigint][coverage]") {
    BigInteger source(1234);
    BigInteger moved(std::move(source));
    REQUIRE(moved.to_string() == "1234");
    REQUIRE(source.is_zero());

    BigInteger assigned(1);
    assigned = std::move(moved);
    REQUIRE(assigned.to_string() == "1234");
    REQUIRE(moved.is_zero());
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

TEST_CASE("LicenseValidator rejects undecodable payload", "[crypto][license]") {
    LicenseValidator validator;
    REQUIRE(validator.validate("###.sig") == LicenseStatus::InvalidFormat);
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

TEST_CASE("LicenseValidator invalid public key keeps valid payload unsigned",
          "[crypto][license][coverage][phase3]") {
    LicenseValidator validator;
    validator.set_public_key("not a pem public key");

    std::string payload = "{\"product_id\":\"PulpSynth\",\"issued\":1700000000}";
    std::string key = base64_encode(payload) + "." + base64_encode("signature");

    REQUIRE(validator.validate(key) == LicenseStatus::InvalidSignature);
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

TEST_CASE("LicenseValidator validate_and_parse includes optional machine and expiry fields",
          "[crypto][license][coverage][issue-656]") {
    LicenseValidator validator;

    std::string payload = "{\"product_id\":\"PulpSuite\",\"email\":\"user@example.com\","
                          "\"machine_id\":\"machine-1\",\"edition\":\"team\","
                          "\"issued\":1700000000,\"expiry\":1800000000}";
    auto info = validator.validate_and_parse(base64_encode(payload) + ".sig");
    REQUIRE(info.has_value());
    REQUIRE(info->product_id == "PulpSuite");
    REQUIRE(info->user_email == "user@example.com");
    REQUIRE(info->machine_id == "machine-1");
    REQUIRE(info->edition == "team");
    REQUIRE(info->issued_timestamp == 1700000000);
    REQUIRE(info->expiry_timestamp == 1800000000);
}

TEST_CASE("LicenseValidator validate_and_parse rejects malformed payloads", "[crypto][license]") {
    LicenseValidator validator;

    REQUIRE_FALSE(validator.validate_and_parse("missing-dot").has_value());
    REQUIRE_FALSE(validator.validate_and_parse("###.sig").has_value());

    std::string missing_product = "{\"email\":\"user@example.com\",\"issued\":1700000000}";
    REQUIRE_FALSE(validator.validate_and_parse(base64_encode(missing_product) + ".sig").has_value());
}

TEST_CASE("LicenseValidator parse_payload handles optional fields and bad integers",
          "[crypto][license][coverage]") {
    LicenseValidator validator;

    std::string payload =
        "{\"product_id\":\"PulpSynth\",\"email\":\"user@example.com\","
        "\"machine_id\":\"machine-1\",\"edition\":\"artist\","
        "\"issued\":not-a-number,\"expiry\":1800000000}";
    auto info = validator.validate_and_parse(base64_encode(payload) + ".sig");

    REQUIRE(info.has_value());
    REQUIRE(info->product_id == "PulpSynth");
    REQUIRE(info->user_email == "user@example.com");
    REQUIRE(info->machine_id == "machine-1");
    REQUIRE(info->edition == "artist");
    REQUIRE(info->issued_timestamp == 0);
    REQUIRE(info->expiry_timestamp == 1800000000);
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

TEST_CASE("LicenseValidator validate_file trims line endings", "[crypto][license]") {
    TemporaryFile tmp(".license");
    std::string payload = "{\"product_id\":\"PulpSynth\",\"issued\":1700000000}";
    std::string key = base64_encode(payload) + "." + base64_encode("signature") + "\r\n";

    {
        std::ofstream out(tmp.path());
        REQUIRE(out.good());
        out << key;
    }

    LicenseValidator validator;
    REQUIRE(validator.validate_file(tmp.path_string()) == LicenseStatus::InvalidSignature);
}

TEST_CASE("LicenseValidator validate_file trims repeated CRLF endings",
          "[crypto][license][coverage][phase3]") {
    TemporaryFile tmp(".license");
    std::string payload = "{\"product_id\":\"PulpSynth\",\"issued\":1700000000}";
    std::string key = base64_encode(payload) + "." + base64_encode("signature") + "\r\n\n";

    {
        std::ofstream out(tmp.path());
        REQUIRE(out.good());
        out << key;
    }

    LicenseValidator validator;
    REQUIRE(validator.validate_file(tmp.path_string()) == LicenseStatus::InvalidSignature);
}

TEST_CASE("LicenseValidator validate_file accepts file without trailing newline",
          "[crypto][license][issue-641]") {
    TemporaryFile tmp(".license");
    std::string payload = "{\"product_id\":\"PulpSynth\",\"issued\":1700000000}";
    std::string key = base64_encode(payload) + "." + base64_encode("signature");

    {
        std::ofstream out(tmp.path());
        REQUIRE(out.good());
        out << key;
    }

    LicenseValidator validator;
    REQUIRE(validator.validate_file(tmp.path_string()) == LicenseStatus::InvalidSignature);
}

TEST_CASE("LicenseValidator validate rejects empty payload section",
          "[crypto][license][issue-641]") {
    LicenseValidator validator;
    REQUIRE(validator.validate(".sig") == LicenseStatus::InvalidSignature);
}

TEST_CASE("LicenseValidator validate rejects malformed signature encoding",
          "[crypto][license][coverage][phase3-large]") {
    LicenseValidator validator;
    std::string payload = "{\"product_id\":\"PulpSynth\",\"issued\":1700000000}";
    REQUIRE(validator.validate(base64_encode(payload) + ".###") == LicenseStatus::InvalidSignature);
}

TEST_CASE("LicenseValidator validate rejects decoded payload missing product id",
          "[crypto][license][coverage][phase3-large]") {
    LicenseValidator validator;
    std::string payload = "{\"email\":\"user@example.com\",\"issued\":1700000000}";
    REQUIRE(validator.validate(base64_encode(payload) + "." + base64_encode("sig")) ==
            LicenseStatus::InvalidSignature);
    REQUIRE_FALSE(validator.validate_and_parse(base64_encode(payload) + ".sig"));
}

TEST_CASE("LicenseValidator validate_and_parse accepts minimal product payload",
          "[crypto][license][coverage][phase3-large]") {
    LicenseValidator validator;
    auto info = validator.validate_and_parse(base64_encode("{\"product_id\":\"PulpMini\"}") + ".sig");
    REQUIRE(info.has_value());
    REQUIRE(info->product_id == "PulpMini");
    REQUIRE(info->user_email.empty());
    REQUIRE(info->issued_timestamp == 0);
    REQUIRE(info->expiry_timestamp == 0);
}

TEST_CASE("LicenseValidator validate_and_parse ignores malformed optional integers",
          "[crypto][license][coverage][phase3-large]") {
    LicenseValidator validator;
    std::string payload = "{\"product_id\":\"PulpMini\",\"expiry\":not-a-number}";
    auto info = validator.validate_and_parse(base64_encode(payload) + ".sig");
    REQUIRE(info.has_value());
    REQUIRE(info->expiry_timestamp == 0);
}

TEST_CASE("LicenseValidator validate_file preserves interior whitespace",
          "[crypto][license][coverage][phase3-large]") {
    TemporaryFile tmp(".license");
    std::string payload = "{\"product_id\":\"PulpSynth\"}";
    std::string key = base64_encode(payload) + "." + base64_encode("sig") + "\n\n";

    {
        std::ofstream out(tmp.path());
        REQUIRE(out.good());
        out << key;
    }

    LicenseValidator validator;
    REQUIRE(validator.validate_file(tmp.path_string()) == LicenseStatus::InvalidSignature);
}

TEST_CASE("LicenseValidator machine check accepts copied machine id",
          "[crypto][license][coverage][phase3-large]") {
    LicenseValidator validator;
    LicenseInfo info;
    info.machine_id = std::string(machine_id());
    REQUIRE(validator.is_valid_for_machine(info));
}

TEST_CASE("LicenseGenerator emits nullopt for empty private key with complete info",
          "[crypto][license][coverage][phase3-large]") {
    LicenseInfo info;
    info.product_id = "PulpSynth";
    info.user_email = "user@example.com";
    info.machine_id = "machine";
    info.edition = "pro";
    info.issued_timestamp = 1700000000;
    info.expiry_timestamp = 1800000000;

    LicenseGenerator generator;
    REQUIRE_FALSE(generator.generate(info));
}

TEST_CASE("OnlineActivation treats empty server URL as failed request",
          "[crypto][license][coverage][phase3-large]") {
    REQUIRE_FALSE(OnlineActivation::activate("", "serial", "product"));
    REQUIRE_FALSE(OnlineActivation::deactivate("", "license"));
    REQUIRE(OnlineActivation::check_status("", "license") == LicenseStatus::NotFound);
}

TEST_CASE("LicenseGenerator requires a usable private key", "[crypto][license]") {
    LicenseInfo info;
    info.product_id = "PulpSynth";
    info.user_email = "user@example.com";
    info.issued_timestamp = 1700000000;

    LicenseGenerator generator;
    REQUIRE_FALSE(generator.generate(info).has_value());

    generator.set_private_key("not a pem key");
    REQUIRE_FALSE(generator.generate(info).has_value());
}

TEST_CASE("OnlineActivation rejects malformed server URLs without network", "[crypto][license][coverage][issue-656]") {
    REQUIRE_FALSE(OnlineActivation::activate("not-a-url", "serial", "product").has_value());
    REQUIRE_FALSE(OnlineActivation::deactivate("not-a-url", "license"));
    REQUIRE(OnlineActivation::check_status("not-a-url", "license") == LicenseStatus::NotFound);
}
