#pragma once

/// Control-manifest fixture text and shipping markers derived from the
/// canonical registry digest.
///
/// The registry digest is a hash of the capability registry, so a manifest that
/// embeds it as a literal has to be re-typed by hand every time the registry
/// moves. These constants splice the canonical value in instead, and the
/// shipping markers are the compile-time SHA-256 of the very bytes a test
/// writes beside its fixture, so the digest stays checked in exactly once.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#if defined(PULP_CONTROL_REGISTRY_DIGEST_V1)
#undef PULP_CONTROL_REGISTRY_DIGEST_V1
#endif
#include <pulp/inspect/control_registry_digest.inc>

namespace pulp::test {

inline constexpr std::string_view kTrustedHostFixtureManifest = R"({
  "schema": "dev.pulp.control/artifact-manifest@1",
  "schema_version": 1,
  "profile": "developer-local",
  "target": "pulp-control-trusted-host-fixture",
  "product_name": "Pulp Trusted Host Fixture",
  "bundle_id": "dev.pulp.test.trusted-host-fixture",
  "build_id": "build:0123456789abcdef0123456789abcdef",
  "registry_digest": ")" PULP_CONTROL_REGISTRY_DIGEST_V1 R"(",
  "endpoint_included": true,
  "unsafe_runtime_eval_acknowledged": false,
  "permission_terms": ["implemented", "built", "host_available", "activated", "policy_eligible", "client_granted", "session_live"],
  "capabilities": ["dev.pulp.instance/read@1"]
}
)";

inline constexpr std::string_view kTrustedHostE2eFixtureManifest = R"({
  "schema": "dev.pulp.control/artifact-manifest@1",
  "schema_version": 1,
  "profile": "developer-local",
  "target": "pulp-control-trusted-host-e2e-fixture",
  "product_name": "Pulp Trusted Host E2E Fixture",
  "bundle_id": "dev.pulp.test.trusted-host-e2e-fixture",
  "build_id": "build:0123456789abcdef0123456789abcdef",
  "registry_digest": ")" PULP_CONTROL_REGISTRY_DIGEST_V1 R"(",
  "endpoint_included": true,
  "unsafe_runtime_eval_acknowledged": false,
  "permission_terms": ["implemented", "built", "host_available", "activated", "policy_eligible", "client_granted", "session_live"],
  "capabilities": ["dev.pulp.instance/read@1", "dev.pulp.session/control@1", "dev.pulp.trace/session-control@1"]
}
)";

inline constexpr std::string_view kInstalledHostE2eFixtureManifest = R"({
  "schema": "dev.pulp.control/artifact-manifest@1",
  "schema_version": 1,
  "profile": "developer-local",
  "target": "pulp-control-installed-host-e2e-fixture",
  "product_name": "Pulp Installed Host E2E Fixture",
  "bundle_id": "dev.pulp.test.installed-host-e2e-fixture",
  "build_id": "build:1123456789abcdef0123456789abcdef",
  "registry_digest": ")" PULP_CONTROL_REGISTRY_DIGEST_V1 R"(",
  "endpoint_included": true,
  "unsafe_runtime_eval_acknowledged": false,
  "permission_terms": ["implemented", "built", "host_available", "activated", "policy_eligible", "client_granted", "session_live"],
  "capabilities": ["dev.pulp.session/control@1", "dev.pulp.trace/control@1", "dev.pulp.trace/session-control@1", "dev.pulp.ui/input@1"]
}
)";

namespace detail {

inline constexpr std::uint32_t kSha256Constants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

constexpr std::uint32_t rotate_right(std::uint32_t value, unsigned bits) {
    return (value >> bits) | (value << (32u - bits));
}

/// SHA-256 over `input`, evaluated while compiling so a fixture marker can be
/// spelled as the hash of the manifest it belongs to rather than as a literal.
/// `test_control_manifest.cpp` pins this against `pulp::runtime::sha256_hex`.
constexpr std::array<unsigned char, 32> sha256(std::string_view input) {
    std::uint32_t state[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                              0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    const std::size_t length = input.size();
    const std::uint64_t bit_length = static_cast<std::uint64_t>(length) * 8ull;
    const std::size_t padded = ((length + 9u + 63u) / 64u) * 64u;
    for (std::size_t block = 0; block < padded; block += 64u) {
        std::uint32_t schedule[64] = {};
        for (std::size_t offset = 0; offset < 64u; ++offset) {
            const std::size_t index = block + offset;
            unsigned char byte = 0;
            if (index < length)
                byte = static_cast<unsigned char>(input[index]);
            else if (index == length)
                byte = 0x80u;
            else if (index >= padded - 8u)
                byte = static_cast<unsigned char>((bit_length >> (8u * (padded - 1u - index))) &
                                                 0xffull);
            schedule[offset / 4u] |= static_cast<std::uint32_t>(byte) << (24u - 8u * (offset % 4u));
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotate_right(schedule[i - 15], 7) ^
                                     rotate_right(schedule[i - 15], 18) ^ (schedule[i - 15] >> 3);
            const std::uint32_t s1 = rotate_right(schedule[i - 2], 17) ^
                                     rotate_right(schedule[i - 2], 19) ^ (schedule[i - 2] >> 10);
            schedule[i] = schedule[i - 16] + s0 + schedule[i - 7] + s1;
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (std::size_t i = 0; i < 64; ++i) {
            const std::uint32_t sigma1 =
                rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
            const std::uint32_t choice = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + sigma1 + choice + kSha256Constants[i] + schedule[i];
            const std::uint32_t sigma0 =
                rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
            const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = sigma0 + majority;
            h = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    }
    std::array<unsigned char, 32> digest{};
    for (std::size_t i = 0; i < 8; ++i) {
        digest[4u * i + 0u] = static_cast<unsigned char>((state[i] >> 24) & 0xffu);
        digest[4u * i + 1u] = static_cast<unsigned char>((state[i] >> 16) & 0xffu);
        digest[4u * i + 2u] = static_cast<unsigned char>((state[i] >> 8) & 0xffu);
        digest[4u * i + 3u] = static_cast<unsigned char>(state[i] & 0xffu);
    }
    return digest;
}

} // namespace detail

/// A shipping marker is scanned out of the built binary, so it has to be static
/// bytes in the image rather than something assembled at run time. Holding it in
/// an aggregate lets a `const volatile` fixture object copy a compile-time value
/// while staying constant-initialised and surviving `-dead_strip`.
struct ControlManifestMarker {
    /// "PULP_CONTROL_MANIFEST_SHA256_" (29) + hex digest (64) + "_V1" (3) + NUL.
    char bytes[97];
};

constexpr ControlManifestMarker control_manifest_marker(std::string_view manifest) {
    constexpr std::string_view prefix = "PULP_CONTROL_MANIFEST_SHA256_";
    constexpr std::string_view suffix = "_V1";
    constexpr char hex_digits[] = "0123456789abcdef";
    ControlManifestMarker marker{};
    std::size_t out = 0;
    for (const char character : prefix)
        marker.bytes[out++] = character;
    for (const unsigned char byte : detail::sha256(manifest)) {
        marker.bytes[out++] = hex_digits[byte >> 4];
        marker.bytes[out++] = hex_digits[byte & 0x0fu];
    }
    for (const char character : suffix)
        marker.bytes[out++] = character;
    marker.bytes[out] = '\0';
    return marker;
}

inline constexpr ControlManifestMarker kTrustedHostFixtureMarker =
    control_manifest_marker(kTrustedHostFixtureManifest);
inline constexpr ControlManifestMarker kTrustedHostE2eFixtureMarker =
    control_manifest_marker(kTrustedHostE2eFixtureManifest);
inline constexpr ControlManifestMarker kInstalledHostE2eFixtureMarker =
    control_manifest_marker(kInstalledHostE2eFixtureManifest);

} // namespace pulp::test

#undef PULP_CONTROL_REGISTRY_DIGEST_V1
