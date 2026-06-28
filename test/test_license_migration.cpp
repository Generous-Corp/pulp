// test_license_migration.cpp — v1 (RSA) -> v2 (AES-256-GCM) license
// format migration coverage.
//
// Covers:
//   1. Format detection (v1 vs v2 vs garbage)
//   2. v2 round-trip with auto-selected generate() path
//   3. v2 round-trip with explicit generate_v2() / shared-secret validator
//   4. v1 still validates against an RSA public key (backward compat)
//   5. migrate_v1_to_v2() preserves LicenseInfo and yields a valid v2 key
//   6. Tampered v1 signature refuses to migrate
//   7. Tampered v2 ciphertext fails authentication
//   8. v2 without a shared secret on the validator returns InvalidSignature
//   9. Different IVs produce different ciphertexts for the same payload

#include <catch2/catch_test_macros.hpp>
#include <pulp/runtime/license.hpp>
#include <pulp/runtime/base64.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace pulp::runtime;

namespace {

// A 2048-bit RSA test keypair generated specifically for the unit tests.
// The private key never leaves this file; the public key is the only thing
// that would be embedded in a real plugin.
//
// Generated once with:
//   openssl genrsa -out priv.pem 2048
//   openssl rsa -in priv.pem -pubout -out pub.pem
//
// Storing both halves inline keeps the test self-contained and free of
// I/O side effects.

// Re-use the same RSA test pair as test_license.cpp so we know mbedTLS
// accepts the PEM format used elsewhere in the suite.
constexpr const char* kTestRsaPrivatePem = R"PEM(-----BEGIN PRIVATE KEY-----
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
-----END PRIVATE KEY-----)PEM";

constexpr const char* kTestRsaPublicPem = R"PEM(-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAoHW3GXDQSt1Es2nX6GZd
W4VKt+zqOJ79E5w8yP0IOKE/HoNqJJqILI5hWg79zDuj6KZoh+zBMIAb9zAc5Uyp
t+YzGMWUVwBivsbvadrl8DcTMC+5TcMAVUVqF9My/rVauJK6JQgcSl9bPN9EmR2n
1kBtLj4rWvxLgzZyz/9/q/+ZfbxTDHI1G6Ffrh1DfWAxlLDfl0fYbe9L/vW6MkqU
t/UH++OKg2lX+DuTbrre+hlbGBKSDHh4HHQ57DzbgXxoRK+liis6fxd+jwQh+m44
pz0KUf2QWk6ysGznAv+qm1OXyj6Py8bL8gxuIb1xg2emy31rwHF0eMVQnBN0t6ka
4QIDAQAB
-----END PUBLIC KEY-----)PEM";

LicenseInfo make_info() {
    LicenseInfo i;
    i.product_id = "com.example.pulp.testpluck";
    i.user_email = "tester@example.com";
    i.edition    = "pro";
    i.issued_timestamp = 1700000000;
    i.expiry_timestamp = 9999999999;  // far future
    return i;
}

std::array<uint8_t, 32> make_secret(uint8_t fill = 0x42) {
    std::array<uint8_t, 32> s{};
    s.fill(fill);
    return s;
}

} // namespace

TEST_CASE("License format detection identifies v1 vs v2 vs garbage",
          "[license][migration][v2]") {
    REQUIRE(detect_license_format("v2.AAAA") == LicenseFormatVersion::V2);
    REQUIRE(detect_license_format("abc.def") == LicenseFormatVersion::V1);
    // v1 historically accepted edge cases like a trailing-dot payload
    // (see test_license.cpp "validate_and_parse ignores an empty signature
    // section"). Treat any non-v2 string containing a '.' as v1; the
    // validate_v1 path then surfaces empty-section cases as
    // InvalidSignature.
    REQUIRE(detect_license_format(".justatail") == LicenseFormatVersion::V1);
    REQUIRE(detect_license_format("leadingdot.") == LicenseFormatVersion::V1);
    REQUIRE_FALSE(detect_license_format("").has_value());
    REQUIRE_FALSE(detect_license_format("nodelimiter").has_value());
}

TEST_CASE("v2 license generate -> validate round-trip",
          "[license][migration][v2]") {
    auto secret = make_secret(0xAB);
    LicenseGenerator gen;
    gen.set_shared_secret(secret.data(), secret.size());

    auto info = make_info();
    auto key = gen.generate_v2(info);
    REQUIRE(key.has_value());
    REQUIRE(detect_license_format(*key) == LicenseFormatVersion::V2);

    LicenseValidator val;
    val.set_shared_secret(secret.data(), secret.size());
    REQUIRE(val.validate(*key) == LicenseStatus::Valid);

    auto parsed = val.validate_and_parse(*key);
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->product_id == info.product_id);
    REQUIRE(parsed->user_email == info.user_email);
    REQUIRE(parsed->edition    == info.edition);
    REQUIRE(parsed->issued_timestamp == info.issued_timestamp);
    REQUIRE(parsed->expiry_timestamp == info.expiry_timestamp);
}

TEST_CASE("v2 license validator rejects key when shared secret is missing",
          "[license][migration][v2]") {
    auto secret = make_secret(0xCD);
    LicenseGenerator gen;
    gen.set_shared_secret(secret.data(), secret.size());
    auto key = gen.generate_v2(make_info());
    REQUIRE(key.has_value());

    LicenseValidator val;  // no secret set
    REQUIRE(val.validate(*key) == LicenseStatus::InvalidSignature);
    REQUIRE_FALSE(val.validate_and_parse(*key).has_value());
}

TEST_CASE("v2 license validator rejects wrong shared secret",
          "[license][migration][v2]") {
    auto secret_a = make_secret(0x11);
    auto secret_b = make_secret(0x22);

    LicenseGenerator gen;
    gen.set_shared_secret(secret_a.data(), secret_a.size());
    auto key = gen.generate_v2(make_info());
    REQUIRE(key.has_value());

    LicenseValidator val;
    val.set_shared_secret(secret_b.data(), secret_b.size());
    REQUIRE(val.validate(*key) == LicenseStatus::InvalidSignature);
}

TEST_CASE("v2 license tamper-detection (ciphertext byte flip)",
          "[license][migration][v2]") {
    auto secret = make_secret(0x77);
    LicenseGenerator gen;
    gen.set_shared_secret(secret.data(), secret.size());
    auto key = gen.generate_v2(make_info());
    REQUIRE(key.has_value());

    // Decode the base64 body, flip a byte in the middle, re-encode.
    std::string body(*key, 3);                          // strip "v2."
    auto bytes = base64_decode(body);
    REQUIRE(bytes.has_value());
    REQUIRE(bytes->size() > 20);
    (*bytes)[15] ^= 0x01;
    std::string tampered = "v2." + base64_encode(bytes->data(), bytes->size());

    LicenseValidator val;
    val.set_shared_secret(secret.data(), secret.size());
    REQUIRE(val.validate(tampered) == LicenseStatus::InvalidSignature);
}

TEST_CASE("v2 license uses fresh IV per generation",
          "[license][migration][v2]") {
    auto secret = make_secret(0x33);
    LicenseGenerator gen;
    gen.set_shared_secret(secret.data(), secret.size());

    auto info = make_info();
    auto a = gen.generate_v2(info);
    auto b = gen.generate_v2(info);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    // Same plaintext input but different IV -> different ciphertext.
    REQUIRE(*a != *b);
}

TEST_CASE("v2 default generate() prefers v2 when shared secret is set",
          "[license][migration][v2]") {
    auto secret = make_secret(0x55);
    LicenseGenerator gen;
    gen.set_shared_secret(secret.data(), secret.size());

    auto key = gen.generate(make_info());
    REQUIRE(key.has_value());
    REQUIRE(detect_license_format(*key) == LicenseFormatVersion::V2);
}

TEST_CASE("v1 license backward-compat round-trip still works",
          "[license][migration][v1]") {
    LicenseGenerator gen;
    gen.set_private_key(kTestRsaPrivatePem);
    auto key = gen.generate_v1(make_info());
    REQUIRE(key.has_value());
    REQUIRE(detect_license_format(*key) == LicenseFormatVersion::V1);

    LicenseValidator val;
    val.set_public_key(kTestRsaPublicPem);
    REQUIRE(val.validate(*key) == LicenseStatus::Valid);
}

TEST_CASE("v1 default generate() falls back to v1 when only RSA key is set",
          "[license][migration][v1]") {
    LicenseGenerator gen;
    gen.set_private_key(kTestRsaPrivatePem);  // no shared secret
    auto key = gen.generate(make_info());
    REQUIRE(key.has_value());
    REQUIRE(detect_license_format(*key) == LicenseFormatVersion::V1);
}

TEST_CASE("migrate_v1_to_v2 preserves LicenseInfo and produces valid v2",
          "[license][migration]") {
    // Issue a v1 key with the RSA private key.
    LicenseGenerator gen_v1;
    gen_v1.set_private_key(kTestRsaPrivatePem);
    auto v1_key = gen_v1.generate_v1(make_info());
    REQUIRE(v1_key.has_value());

    // Migrate to v2 using a fresh shared secret.
    auto secret = make_secret(0x99);
    auto v2_key = migrate_v1_to_v2(*v1_key, kTestRsaPublicPem,
                                   secret.data(), secret.size());
    REQUIRE(v2_key.has_value());
    REQUIRE(detect_license_format(*v2_key) == LicenseFormatVersion::V2);

    // The v2 key validates against the shared secret.
    LicenseValidator val;
    val.set_shared_secret(secret.data(), secret.size());
    REQUIRE(val.validate(*v2_key) == LicenseStatus::Valid);

    // And the round-tripped payload matches the original.
    auto parsed = val.validate_and_parse(*v2_key);
    auto original = make_info();
    REQUIRE(parsed.has_value());
    REQUIRE(parsed->product_id == original.product_id);
    REQUIRE(parsed->user_email == original.user_email);
    REQUIRE(parsed->edition    == original.edition);
    REQUIRE(parsed->issued_timestamp == original.issued_timestamp);
    REQUIRE(parsed->expiry_timestamp == original.expiry_timestamp);
}

TEST_CASE("migrate_v1_to_v2 refuses a tampered v1 signature",
          "[license][migration]") {
    LicenseGenerator gen_v1;
    gen_v1.set_private_key(kTestRsaPrivatePem);
    auto v1_key = gen_v1.generate_v1(make_info());
    REQUIRE(v1_key.has_value());

    // Flip a byte inside the signature half (after the '.').
    auto dot = v1_key->find('.');
    REQUIRE(dot != std::string::npos);
    std::string tampered = *v1_key;
    REQUIRE(dot + 4 < tampered.size());
    tampered[dot + 4] = (tampered[dot + 4] == 'A') ? 'B' : 'A';

    auto secret = make_secret(0xAA);
    auto v2_key = migrate_v1_to_v2(tampered, kTestRsaPublicPem,
                                   secret.data(), secret.size());
    REQUIRE_FALSE(v2_key.has_value());
}

TEST_CASE("migrate_v1_to_v2 rejects a v2 key as input",
          "[license][migration]") {
    auto secret = make_secret(0xEE);
    LicenseGenerator gen_v2;
    gen_v2.set_shared_secret(secret.data(), secret.size());
    auto v2_key = gen_v2.generate_v2(make_info());
    REQUIRE(v2_key.has_value());

    // Passing a v2 key to the migration helper must fail — it only
    // accepts v1 input.
    auto result = migrate_v1_to_v2(*v2_key, kTestRsaPublicPem,
                                   secret.data(), secret.size());
    REQUIRE_FALSE(result.has_value());
}

TEST_CASE("migrate_v1_to_v2 rejects wrong-size shared secret",
          "[license][migration]") {
    LicenseGenerator gen_v1;
    gen_v1.set_private_key(kTestRsaPrivatePem);
    auto v1_key = gen_v1.generate_v1(make_info());
    REQUIRE(v1_key.has_value());

    std::vector<uint8_t> short_secret(16, 0xBB);  // 128-bit, not 256
    REQUIRE_FALSE(migrate_v1_to_v2(*v1_key, kTestRsaPublicPem,
                                   short_secret.data(),
                                   short_secret.size()).has_value());
}
