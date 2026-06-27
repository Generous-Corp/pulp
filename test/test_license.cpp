#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/license.hpp>
#include <pulp/runtime/big_integer.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/runtime/socket.hpp>
#include <pulp/runtime/temporary_file.hpp>
#include "../external/cpp-httplib/httplib.h"

#include <array>
#include <chrono>
#include <ctime>
#include <fstream>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

using namespace pulp::runtime;
using namespace std::chrono_literals;

namespace {

struct LicenseHttpServerRunner {
    explicit LicenseHttpServerRunner(httplib::Server& s)
        : server(s)
        , thread([this] { server.listen_after_bind(); }) {}

    ~LicenseHttpServerRunner() {
        server.stop();
        if (thread.joinable()) {
            thread.join();
        }
    }

    bool wait_until_running(std::chrono::milliseconds timeout = 2s) const {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!server.is_running() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        return server.is_running();
    }

    httplib::Server& server;
    std::thread thread;
};

}  // namespace

namespace {

struct LoopbackHttpExchange {
    std::string base_url;
    std::shared_ptr<std::string> request = std::make_shared<std::string>();
    std::thread worker;

    LoopbackHttpExchange() = default;
    LoopbackHttpExchange(const LoopbackHttpExchange&) = delete;
    LoopbackHttpExchange& operator=(const LoopbackHttpExchange&) = delete;
    LoopbackHttpExchange(LoopbackHttpExchange&&) noexcept = default;
    LoopbackHttpExchange& operator=(LoopbackHttpExchange&&) noexcept = default;

    ~LoopbackHttpExchange() {
        if (worker.joinable()) worker.join();
    }
};

LoopbackHttpExchange serve_loopback_http_response(std::string response_body) {
    Socket server;
    REQUIRE(server.create(SocketType::TCP));
    REQUIRE(server.bind("127.0.0.1", 0));
    REQUIRE(server.listen(1));
    const auto port = server.local_port();
    REQUIRE(port != 0);

    LoopbackHttpExchange exchange;
    exchange.base_url = "http://127.0.0.1:" + std::to_string(port);
    auto request = exchange.request;
    exchange.worker = std::thread([server = std::move(server),
                                   response_body = std::move(response_body),
                                   request = std::move(request)]() mutable {
        auto client = server.accept();
        if (!client) return;

        std::array<std::uint8_t, 2048> buffer{};
        while (true) {
            const auto received = client->receive(buffer.data(), buffer.size());
            if (received <= 0) break;
            request->append(reinterpret_cast<const char*>(buffer.data()),
                            static_cast<std::size_t>(received));

            const auto header_end = request->find("\r\n\r\n");
            if (header_end == std::string::npos) continue;

            std::size_t expected_body_size = 0;
            const auto length_key = std::string("Content-Length: ");
            const auto length_pos = request->find(length_key);
            if (length_pos != std::string::npos) {
                const auto start = length_pos + length_key.size();
                const auto end = request->find("\r\n", start);
                expected_body_size =
                    static_cast<std::size_t>(std::stoul(request->substr(start, end - start)));
            }

            const auto body_size = request->size() - (header_end + 4);
            if (body_size >= expected_body_size) break;
        }

        const std::string wire_response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(response_body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + response_body;
        (void) client->send(wire_response);
    });
    return exchange;
}

LoopbackHttpExchange serve_loopback_http_status(int status_code, std::string response_body = {}) {
    Socket server;
    REQUIRE(server.create(SocketType::TCP));
    REQUIRE(server.bind("127.0.0.1", 0));
    REQUIRE(server.listen(1));
    const auto port = server.local_port();
    REQUIRE(port != 0);

    LoopbackHttpExchange exchange;
    exchange.base_url = "http://127.0.0.1:" + std::to_string(port);
    auto request = exchange.request;
    exchange.worker = std::thread([server = std::move(server),
                                   status_code,
                                   response_body = std::move(response_body),
                                   request = std::move(request)]() mutable {
        auto client = server.accept();
        if (!client) return;

        std::array<std::uint8_t, 2048> buffer{};
        const auto received = client->receive(buffer.data(), buffer.size());
        if (received > 0) {
            request->assign(reinterpret_cast<const char*>(buffer.data()),
                            static_cast<std::size_t>(received));
        }

        const auto reason = status_code >= 200 && status_code < 300 ? "OK" : "Rejected";
        const std::string wire_response =
            "HTTP/1.1 " + std::to_string(status_code) + " " + reason + "\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: " + std::to_string(response_body.size()) + "\r\n"
            "Connection: close\r\n\r\n" + response_body;
        (void) client->send(wire_response);
    });
    return exchange;
}

constexpr std::string_view kTestPrivateKey = R"(-----BEGIN PRIVATE KEY-----
MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQCgdbcZcNBK3USz
adfoZl1bhUq37Oo4nv0TnDzI/Qg4oT8eg2okmogsjmFaDv3MO6PopmiH7MEwgBv3
MBzlTKm35jMYxZRXAGK+xu9p2uXwNxMwL7lNwwBVRWoX0zL+tVq4krolCBxKX1s8
30SZHafWQG0uPita/EuDNnLP/3+r/5l9vFMMcjUboV+uHUN9YDGUsN+XR9ht70v+
9boySpS39Qf744qDaVf4O5Nuut76GVsYEpIMeHgcdDnsPNuBfGhEr6WKKzp/F36P
BCH6bjinPQpR/ZBaTrKwbOcC/6qbU5fKPo/LxsvyDG4hvXGDZ6bLfWvAcXR4xVCc
E3S3qRrhAgMBAAECggEAMv5mA5hGIdyi9i+ndYx+k9TO85e/seHZBM/sw2UipTid
YhmadGqF5z8Scjf8cVjs1MV5x+S2Wq8D9DEepcKQ10g5qeA0rdeKCh4XvPjbhVVD
bFdmWO+lXfQS7OJqPOcunyTGMnma4Ang6X39A3oYui68Y+tPBPnUF62InFCS5vpx
dDVa7HehBS22vlOV2K/G6A+j4DKYzcRJ8KObQIxcbLzzrqiJD294xiCcpYK6JKKE
67CyYQaznYQMC5yFzHKpJR/7NLt/UaX1uNuuSuvIVMQkWpUhUpPAvcdlCKF3HxwK
YZCPLmb32GFvMcxgCHxXYeHUszBYsVpVVFrK92+DpwKBgQDVTHakuM+OHT+HsL/Z
4DrmH1QdlM6kwXCCJLBvQPZsQiZ/6RFsbBcIs0m+v8lgSidg0VSSCIY3lB4UUsVJ
fhnKRax9hg7TqfiSU4qi8FeyL0ygsRQsbCcSceX+1OpFw08dwqUBlU8lm3YMkGzW
bFg/eHzcWbXOvMHGQ1tys5//OwKBgQDAlUPyPTV/kj09oh8ZVclEgOQhYxjIGDqG
gjn8Z5VT3b6lVPoOTyrTs8WXLO0P2dUcSlLd8/2zJ1rj/4QT+JNNbBdXR7lCH1CN
EwAEoLAds6ne3I/vNU2I9S0W7I7S+WYoKUdzqem0hK1JCYY5IQYgDK+2UmiVUqnf
QnnVdNvkkwKBgDaUhF+OMv5ImbMdFVqpOCkepyWSqHYcUa/bt5Iga240Vymv+Bdo
aVR+nEZFSUBseTsbFarwp3edXT0SGQ2/SNYrkgHkxfJ/se2vlvAu1CHyXIdaCLF9
U1oy7wmQbgo/+gTBg/utuz0CVOjOJCuSOdqz+C9ifVVTk7oqDkKBmFV1AoGBALIy
K6Hcs0Dsrw/1kDMgJtDlNrISAN89VAIcQ81ih1EH0J0pCQvugyFKqd1da6mMFV5N
A2puluSL2NU5HBximOI9z0cqjag6U42F9DFUAkIpeVhG3EQqHSlKO8OHdgyPStCE
iaHjqeUoEzOOxYzdGs7TDk9052KsD5UO7K7vp3xTAoGBAKjA2g1umyp4DeVUW2id
eMr4+ZWnusgqXZk7g+7bfWfbSygFqbK7jMu7FTEbG1zJqxB6ZfcgeiPwoMuU6oSi
m9dK3zogwSFa6HommLGcIaVlG7b/bYyQHi/Prtrn9svqcpofWUcGTVKY/lMGnwRg
dQWvWxSNQzzjPizB/TBexRyq
-----END PRIVATE KEY-----)";

constexpr std::string_view kTestPublicKey = R"(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAoHW3GXDQSt1Es2nX6GZd
W4VKt+zqOJ79E5w8yP0IOKE/HoNqJJqILI5hWg79zDuj6KZoh+zBMIAb9zAc5Uyp
t+YzGMWUVwBivsbvadrl8DcTMC+5TcMAVUVqF9My/rVauJK6JQgcSl9bPN9EmR2n
1kBtLj4rWvxLgzZyz/9/q/+ZfbxTDHI1G6Ffrh1DfWAxlLDfl0fYbe9L/vW6MkqU
t/UH++OKg2lX+DuTbrre+hlbGBKSDHh4HHQ57DzbgXxoRK+liis6fxd+jwQh+m44
pz0KUf2QWk6ysGznAv+qm1OXyj6Py8bL8gxuIb1xg2emy31rwHF0eMVQnBN0t6ka
4QIDAQAB
-----END PUBLIC KEY-----)";

}  // namespace

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
          "[crypto][bigint]") {
    REQUIRE(BigInteger::from_string("not-a-number").is_zero());
    REQUIRE(BigInteger::from_hex("not-hex").is_zero());
    REQUIRE(BigInteger::from_string("").is_zero());
}

TEST_CASE("BigInteger copy move hex and bit-count helpers", "[crypto][bigint][issue-656]") {
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
          "[crypto][bigint]") {
    auto value = BigInteger::from_hex("0100");
    REQUIRE(value.to_hex() == "0100");
    REQUIRE(value.bit_count() == 9);

    BigInteger copy;
    copy = value;
    REQUIRE(copy == value);
    REQUIRE(copy.to_string() == "256");
}

TEST_CASE("BigInteger move construction and move assignment leave values usable",
          "[crypto][bigint]") {
    BigInteger source(1234);
    BigInteger moved(std::move(source));
    REQUIRE(moved.to_string() == "1234");
    REQUIRE(source.is_zero());

    BigInteger assigned(1);
    assigned = std::move(moved);
    REQUIRE(assigned.to_string() == "1234");
    REQUIRE(moved.is_zero());
}

TEST_CASE("BigInteger self assignment and identity arithmetic stay stable",
          "[crypto][bigint]") {
    BigInteger value(144);
    auto& same = value;

    value = same;
    REQUIRE(value.to_string() == "144");

    value = std::move(same);
    REQUIRE(value.to_string() == "144");

    REQUIRE((value + BigInteger(0)).to_string() == "144");
    REQUIRE((value * BigInteger(1)).to_string() == "144");
    REQUIRE((BigInteger(3) % BigInteger(10)).to_string() == "3");
    REQUIRE(value.mod_pow(BigInteger(0), BigInteger(17)).to_string() == "1");
}

TEST_CASE("BigInteger parses case-insensitive hex and decimal leading zeroes",
          "[crypto][bigint]") {
    auto hex = BigInteger::from_hex("00ff");
    REQUIRE(hex.to_string() == "255");
    REQUIRE(hex.to_hex() == "FF");

    auto decimal = BigInteger::from_string("000123");
    REQUIRE(decimal.to_string() == "123");
    REQUIRE(decimal.bit_count() == 7);
}

TEST_CASE("BigInteger uint64 constructor preserves unsigned high values",
          "[crypto][bigint]") {
    BigInteger max_value(std::numeric_limits<std::uint64_t>::max());

    REQUIRE(max_value.to_string() == "18446744073709551615");
    REQUIRE(max_value.to_hex() == "FFFFFFFFFFFFFFFF");
    REQUIRE(max_value.bit_count() == 64);
    REQUIRE_FALSE(max_value.is_zero());
}

TEST_CASE("BigInteger ordering covers equal and greater comparisons",
          "[crypto][bigint]") {
    BigInteger low(99);
    BigInteger same_low(99);
    BigInteger high(100);

    REQUIRE_FALSE(low < same_low);
    REQUIRE_FALSE(high < low);
    REQUIRE(low < high);
    REQUIRE(low == same_low);
    REQUIRE(high != low);
}

TEST_CASE("BigInteger comparison covers equal and greater-than paths",
          "[crypto][bigint]") {
    BigInteger low(255);
    auto same = BigInteger::from_hex("FF");
    BigInteger high(256);

    REQUIRE(low == same);
    REQUIRE_FALSE(low != same);
    REQUIRE_FALSE(same < low);
    REQUIRE_FALSE(high < low);
    REQUIRE(low < high);
    REQUIRE(BigInteger().bit_count() == 0);
}

TEST_CASE("BigInteger preserves negative decimal values through arithmetic",
          "[crypto][bigint]") {
    auto negative = BigInteger::from_string("-42");
    auto positive = BigInteger(50);

    REQUIRE(negative.to_string() == "-42");
    REQUIRE((negative + positive).to_string() == "8");
    REQUIRE((negative * BigInteger(2)).to_string() == "-84");
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

TEST_CASE("LicenseValidator rejects impossible base64 payload lengths",
          "[crypto][license]") {
    LicenseValidator validator;
    REQUIRE(validator.validate("A.sig") == LicenseStatus::InvalidFormat);
    REQUIRE_FALSE(validator.validate_and_parse("A.sig"));
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
          "[crypto][license]") {
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
          "[crypto][license][issue-656]") {
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

TEST_CASE("LicenseValidator validate_and_parse rejects malformed numeric timestamps", "[crypto][license]") {
    LicenseValidator validator;

    auto license_for = [](std::string_view payload) {
        return base64_encode(payload) + ".sig";
    };

    REQUIRE_FALSE(validator.validate_and_parse(
        license_for("{\"product_id\":\"PulpGain\",\"issued\":1700000000junk}")).has_value());
    REQUIRE_FALSE(validator.validate_and_parse(
        license_for("{\"product_id\":\"PulpGain\",\"expiry\":1700000000ms}")).has_value());
    REQUIRE_FALSE(validator.validate_and_parse(
        license_for("{\"product_id\":\"PulpGain\",\"issued\":92233720368547758070}")).has_value());
}

TEST_CASE("LicenseValidator parse_payload handles optional fields and bad integers",
          "[crypto][license]") {
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

TEST_CASE("LicenseValidator empty license file is invalid format",
          "[crypto][license]") {
    TemporaryFile tmp(".license");

    LicenseValidator validator;
    REQUIRE(validator.validate_file(tmp.path_string()) == LicenseStatus::InvalidFormat);
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
          "[crypto][license]") {
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

TEST_CASE("LicenseValidator validate rejects an empty signature section",
          "[crypto][license]") {
    LicenseValidator validator;
    std::string payload = "{\"product_id\":\"PulpSynth\"}";

    REQUIRE(validator.validate(base64_encode(payload) + ".") == LicenseStatus::InvalidSignature);
}

TEST_CASE("LicenseValidator validate rejects malformed signature encoding",
          "[crypto][license]") {
    LicenseValidator validator;
    std::string payload = "{\"product_id\":\"PulpSynth\",\"issued\":1700000000}";
    REQUIRE(validator.validate(base64_encode(payload) + ".###") == LicenseStatus::InvalidSignature);
}

TEST_CASE("LicenseValidator validate rejects decoded payload missing product id",
          "[crypto][license]") {
    LicenseValidator validator;
    std::string payload = "{\"email\":\"user@example.com\",\"issued\":1700000000}";
    REQUIRE(validator.validate(base64_encode(payload) + "." + base64_encode("sig")) ==
            LicenseStatus::InvalidSignature);
    REQUIRE_FALSE(validator.validate_and_parse(base64_encode(payload) + ".sig"));
}

TEST_CASE("LicenseValidator validate_and_parse accepts minimal product payload",
          "[crypto][license]") {
    LicenseValidator validator;
    auto info = validator.validate_and_parse(base64_encode("{\"product_id\":\"PulpMini\"}") + ".sig");
    REQUIRE(info.has_value());
    REQUIRE(info->product_id == "PulpMini");
    REQUIRE(info->user_email.empty());
    REQUIRE(info->issued_timestamp == 0);
    REQUIRE(info->expiry_timestamp == 0);
}

TEST_CASE("LicenseValidator validate_and_parse ignores an empty signature section",
          "[crypto][license]") {
    LicenseValidator validator;
    auto info = validator.validate_and_parse(base64_encode("{\"product_id\":\"PulpParseOnly\"}") + ".");

    REQUIRE(info.has_value());
    REQUIRE(info->product_id == "PulpParseOnly");
}

TEST_CASE("LicenseValidator validate_and_parse keeps first dotted payload split",
          "[crypto][license]") {
    LicenseValidator validator;
    std::string payload = "{\"product_id\":\"PulpDot\"}";
    auto info = validator.validate_and_parse(base64_encode(payload) + ".sig.with.dots");

    REQUIRE(info.has_value());
    REQUIRE(info->product_id == "PulpDot");
}

TEST_CASE("LicenseValidator validate_and_parse rejects partial optional integers",
          "[crypto][license]") {
    LicenseValidator validator;
    std::string payload = "{\"product_id\":\"PulpMini\",\"issued\":123junk,\"expiry\":not-a-number}";
    auto info = validator.validate_and_parse(base64_encode(payload) + ".sig");
    REQUIRE_FALSE(info.has_value());
}

TEST_CASE("LicenseValidator validate_and_parse accepts whitespace after optional integers",
          "[crypto][license]") {
    LicenseValidator validator;
    std::string payload = "{\"product_id\":\"PulpMini\",\"issued\":123 \t,\"expiry\":456 }";
    auto info = validator.validate_and_parse(base64_encode(payload) + ".sig");
    REQUIRE(info.has_value());
    REQUIRE(info->issued_timestamp == 123);
    REQUIRE(info->expiry_timestamp == 456);

    std::string truncated_after_int = "{\"product_id\":\"PulpMini\",\"issued\":789";
    auto truncated_info =
        validator.validate_and_parse(base64_encode(truncated_after_int) + ".sig");
    REQUIRE(truncated_info.has_value());
    REQUIRE(truncated_info->issued_timestamp == 789);
}

TEST_CASE("LicenseValidator validate_and_parse accepts whitespace before optional integers",
          "[crypto][license]") {
    LicenseValidator validator;
    std::string payload = "{\"product_id\":\"PulpMini\",\"issued\": \n\t123,\"expiry\": 456}";
    auto info = validator.validate_and_parse(base64_encode(payload) + ".sig");
    REQUIRE(info.has_value());
    REQUIRE(info->issued_timestamp == 123);
    REQUIRE(info->expiry_timestamp == 456);

    std::string partial =
        "{\"product_id\":\"PulpMini\",\"issued\": 123junk,\"expiry\": 456}";
    REQUIRE_FALSE(validator.validate_and_parse(base64_encode(partial) + ".sig").has_value());
}

TEST_CASE("LicenseValidator validate_file preserves interior whitespace",
          "[crypto][license]") {
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

TEST_CASE("LicenseValidator validate_file preserves leading whitespace",
          "[crypto][license]") {
    TemporaryFile tmp(".license");
    std::string payload = "{\"product_id\":\"PulpSynth\"}";
    std::string key = " " + base64_encode(payload) + "." + base64_encode("sig") + "\n";

    {
        std::ofstream out(tmp.path());
        REQUIRE(out.good());
        out << key;
    }

    LicenseValidator validator;
    REQUIRE(validator.validate_file(tmp.path_string()) == LicenseStatus::InvalidSignature);
}

TEST_CASE("LicenseValidator machine check accepts copied machine id",
          "[crypto][license]") {
    LicenseValidator validator;
    LicenseInfo info;
    info.machine_id = std::string(machine_id());
    REQUIRE(validator.is_valid_for_machine(info));
}

TEST_CASE("LicenseGenerator emits nullopt for empty private key with complete info",
          "[crypto][license]") {
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

TEST_CASE("LicenseGenerator signs complete licenses that validate on this machine",
          "[crypto][license]") {
    LicenseInfo info;
    info.product_id = "PulpSynth";
    info.user_email = "user@example.com";
    info.machine_id = std::string(machine_id());
    info.edition = "pro";
    info.issued_timestamp = 1700000000;
    info.expiry_timestamp = static_cast<int64_t>(std::time(nullptr)) + 3600;

    LicenseGenerator generator;
    generator.set_private_key(kTestPrivateKey);
    auto key = generator.generate(info);
    REQUIRE(key.has_value());
    REQUIRE(key->find('.') != std::string::npos);
    auto dot = key->find('.');
    auto payload_bytes = base64_decode(std::string_view(*key).substr(0, dot));
    REQUIRE(payload_bytes.has_value());
    std::string payload(payload_bytes->begin(), payload_bytes->end());
    REQUIRE(payload.find(R"("product_id":"PulpSynth")") != std::string::npos);
    REQUIRE(payload.find(R"("email":"user@example.com")") != std::string::npos);
    REQUIRE(payload.find(R"("machine_id":)") != std::string::npos);
    REQUIRE(payload.find(R"("edition":"pro")") != std::string::npos);
    REQUIRE(payload.find(R"("expiry":)") != std::string::npos);

    LicenseValidator validator;
    validator.set_public_key(kTestPublicKey);
    REQUIRE(validator.validate(*key) == LicenseStatus::Valid);

    auto parsed = validator.validate_and_parse(*key);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->product_id == info.product_id);
    REQUIRE(parsed->user_email == info.user_email);
    REQUIRE(parsed->machine_id == info.machine_id);
    REQUIRE(parsed->edition == info.edition);
    REQUIRE(parsed->issued_timestamp == info.issued_timestamp);
    REQUIRE(parsed->expiry_timestamp == info.expiry_timestamp);
}

TEST_CASE("LicenseValidator reports signed expiry and machine mismatch statuses",
          "[crypto][license]") {
    LicenseGenerator generator;
    generator.set_private_key(kTestPrivateKey);

    LicenseValidator validator;
    validator.set_public_key(kTestPublicKey);

    LicenseInfo expired;
    expired.product_id = "PulpExpired";
    expired.issued_timestamp = 1;
    expired.expiry_timestamp = 1;
    auto expired_key = generator.generate(expired);
    REQUIRE(expired_key.has_value());
    REQUIRE(validator.validate(*expired_key) == LicenseStatus::Expired);

    LicenseInfo wrong_machine;
    wrong_machine.product_id = "PulpMachine";
    wrong_machine.machine_id = "not-this-machine";
    wrong_machine.issued_timestamp = 1700000000;
    wrong_machine.expiry_timestamp = static_cast<int64_t>(std::time(nullptr)) + 3600;
    auto wrong_machine_key = generator.generate(wrong_machine);
    REQUIRE(wrong_machine_key.has_value());
    REQUIRE(validator.validate(*wrong_machine_key) == LicenseStatus::MachineIdMismatch);
}

TEST_CASE("LicenseGenerator signs minimal licenses without optional fields",
          "[crypto][license]") {
    LicenseInfo info;
    info.product_id = "PulpMiniSigned";
    info.issued_timestamp = 1700000000;

    LicenseGenerator generator;
    generator.set_private_key(kTestPrivateKey);
    auto key = generator.generate(info);
    REQUIRE(key.has_value());

    LicenseValidator validator;
    validator.set_public_key(kTestPublicKey);
    REQUIRE(validator.validate(*key) == LicenseStatus::Valid);

    auto parsed = validator.validate_and_parse(*key);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->product_id == "PulpMiniSigned");
    REQUIRE(parsed->user_email.empty());
    REQUIRE(parsed->machine_id.empty());
    REQUIRE(parsed->edition.empty());
    REQUIRE(parsed->expiry_timestamp == 0);
}

TEST_CASE("LicenseValidator rejects tampered signed payloads",
          "[crypto][license]") {
    LicenseInfo info;
    info.product_id = "PulpTamper";
    info.issued_timestamp = 1700000000;

    LicenseGenerator generator;
    generator.set_private_key(kTestPrivateKey);
    auto key = generator.generate(info);
    REQUIRE(key.has_value());

    auto dot = key->find('.');
    REQUIRE(dot != std::string::npos);

    auto payload = base64_decode(std::string_view(*key).substr(0, dot));
    REQUIRE(payload.has_value());
    std::string tampered_payload(payload->begin(), payload->end());
    tampered_payload.replace(tampered_payload.find("PulpTamper"),
                             std::string{"PulpTamper"}.size(), "PulpChanged");
    std::string tampered_key =
        base64_encode(tampered_payload) + "." + key->substr(dot + 1);

    LicenseValidator validator;
    validator.set_public_key(kTestPublicKey);
    REQUIRE(validator.validate(tampered_key) == LicenseStatus::InvalidSignature);
    auto parsed_tampered = validator.validate_and_parse(tampered_key);
    REQUIRE(parsed_tampered.has_value());
    REQUIRE(parsed_tampered->product_id == "PulpChanged");
}

TEST_CASE("LicenseValidator validate_file accepts signed keys with trailing CRLF",
          "[crypto][license]") {
    LicenseInfo info;
    info.product_id = "PulpFileSigned";
    info.issued_timestamp = 1700000000;

    LicenseGenerator generator;
    generator.set_private_key(kTestPrivateKey);
    auto key = generator.generate(info);
    REQUIRE(key.has_value());

    TemporaryFile tmp(".license");
    {
        std::ofstream out(tmp.path());
        REQUIRE(out.good());
        out << *key << "\r\n";
    }

    LicenseValidator validator;
    validator.set_public_key(kTestPublicKey);
    REQUIRE(validator.validate_file(tmp.path_string()) == LicenseStatus::Valid);
}

TEST_CASE("OnlineActivation treats empty server URL as failed request",
          "[crypto][license]") {
    REQUIRE_FALSE(OnlineActivation::activate("", "serial", "product"));
    REQUIRE_FALSE(OnlineActivation::deactivate("", "license"));
    REQUIRE(OnlineActivation::check_status("", "license") == LicenseStatus::NotFound);
}

TEST_CASE("OnlineActivation activate returns loopback license response",
          "[crypto][license]") {
    auto exchange = serve_loopback_http_response("license-key");

    auto license = OnlineActivation::activate(exchange.base_url, "serial-123", "PulpSynth");
    exchange.worker.join();

    REQUIRE(license.has_value());
    REQUIRE(*license == "license-key");
    REQUIRE(exchange.request->find("POST /activate HTTP/1.1") != std::string::npos);
    REQUIRE(exchange.request->find(R"("serial":"serial-123")") != std::string::npos);
    REQUIRE(exchange.request->find(R"("product":"PulpSynth")") != std::string::npos);
}

TEST_CASE("OnlineActivation deactivate accepts successful loopback response",
          "[crypto][license]") {
    auto exchange = serve_loopback_http_response(R"({"ok":true})");

    REQUIRE(OnlineActivation::deactivate(exchange.base_url, "license-key"));
    exchange.worker.join();

    REQUIRE(exchange.request->find("POST /deactivate HTTP/1.1") != std::string::npos);
    REQUIRE(exchange.request->find(R"("key":"license-key")") != std::string::npos);
}

TEST_CASE("OnlineActivation check_status maps loopback response bodies",
          "[crypto][license]") {
    {
        auto exchange = serve_loopback_http_response(R"({"status":"valid"})");
        REQUIRE(OnlineActivation::check_status(exchange.base_url, "valid-key") == LicenseStatus::Valid);
        exchange.worker.join();
        REQUIRE(exchange.request->find("GET /status?key=valid-key&machine=") != std::string::npos);
    }

    {
        auto exchange = serve_loopback_http_response(R"({"status":"expired"})");
        REQUIRE(OnlineActivation::check_status(exchange.base_url, "expired-key") == LicenseStatus::Expired);
        exchange.worker.join();
        REQUIRE(exchange.request->find("GET /status?key=expired-key&machine=") != std::string::npos);
    }

    {
        auto exchange = serve_loopback_http_response(R"({"status":"rejected"})");
        REQUIRE(OnlineActivation::check_status(exchange.base_url, "rejected-key") ==
                LicenseStatus::InvalidSignature);
        exchange.worker.join();
        REQUIRE(exchange.request->find("GET /status?key=rejected-key&machine=") != std::string::npos);
    }
}

TEST_CASE("OnlineActivation treats non-2xx loopback responses as activation failures",
          "[crypto][license]") {
    {
        auto exchange = serve_loopback_http_status(403, R"({"error":"denied"})");
        REQUIRE_FALSE(OnlineActivation::activate(exchange.base_url, "serial", "product"));
        exchange.worker.join();
        REQUIRE(exchange.request->find("POST /activate HTTP/1.1") != std::string::npos);
    }

    {
        auto exchange = serve_loopback_http_status(500, R"({"error":"server"})");
        REQUIRE_FALSE(OnlineActivation::deactivate(exchange.base_url, "license"));
        exchange.worker.join();
        REQUIRE(exchange.request->find("POST /deactivate HTTP/1.1") != std::string::npos);
    }

    {
        auto exchange = serve_loopback_http_status(404, R"({"status":"valid"})");
        REQUIRE(OnlineActivation::check_status(exchange.base_url, "license") == LicenseStatus::NotFound);
        exchange.worker.join();
        REQUIRE(exchange.request->find("GET /status?key=license&machine=") != std::string::npos);
    }
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

TEST_CASE("OnlineActivation rejects malformed server URLs without network", "[crypto][license][issue-656]") {
    REQUIRE_FALSE(OnlineActivation::activate("not-a-url", "serial", "product").has_value());
    REQUIRE_FALSE(OnlineActivation::deactivate("not-a-url", "license"));
    REQUIRE(OnlineActivation::check_status("not-a-url", "license") == LicenseStatus::NotFound);
}

TEST_CASE("OnlineActivation handles successful loopback activation flows",
          "[crypto][license]") {
    httplib::Server server;
    server.Post("/activate", [](const httplib::Request&, httplib::Response& response) {
        response.set_content("license-key", "text/plain");
    });
    server.Post("/deactivate", [](const httplib::Request&, httplib::Response& response) {
        response.status = 204;
    });
    server.Get("/status", [](const httplib::Request& request, httplib::Response& response) {
        const auto key = request.has_param("key") ? request.get_param_value("key") : "";
        if (key == "license-key") {
            response.set_content(R"({"valid":true})", "application/json");
        } else if (key == "expired-key") {
            response.set_content(R"({"expired":true})", "application/json");
        } else {
            response.set_content(R"({"status":"denied"})", "application/json");
        }
    });

    const int port = server.bind_to_any_port("127.0.0.1");
    REQUIRE(port > 0);
    LicenseHttpServerRunner runner(server);
    REQUIRE(runner.wait_until_running());

    const std::string base_url = "http://127.0.0.1:" + std::to_string(port);
    auto license = OnlineActivation::activate(base_url, "serial-1", "PulpSynth");
    REQUIRE(license.has_value());
    REQUIRE(*license == "license-key");

    REQUIRE(OnlineActivation::deactivate(base_url, *license));
    REQUIRE(OnlineActivation::check_status(base_url, *license) == LicenseStatus::Valid);
    REQUIRE(OnlineActivation::check_status(base_url, "expired-key") == LicenseStatus::Expired);
    REQUIRE(OnlineActivation::check_status(base_url, "unknown-key") ==
            LicenseStatus::InvalidSignature);
}
