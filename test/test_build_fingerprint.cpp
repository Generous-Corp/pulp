#include <catch2/catch_test_macros.hpp>

#include <pulp/format/reload/build_fingerprint.hpp>

#include <cstring>
#include <type_traits>

using namespace pulp::format::reload;

// The fingerprint crosses the shell<->logic boundary BEFORE compatibility is
// known, so it must be a trivially-copyable POD (memcpy-safe, no STL ABI).
static_assert(std::is_trivially_copyable<BuildFingerprint>::value,
              "BuildFingerprint must be trivially copyable to cross the seam");
static_assert(std::is_standard_layout<BuildFingerprint>::value,
              "BuildFingerprint must be standard-layout");

TEST_CASE("current_build_fingerprint is populated and self-consistent",
          "[hot-reload][fingerprint]") {
    auto fp = current_build_fingerprint();
    REQUIRE(fp.schema_version == kBuildFingerprintSchema);
    REQUIRE(fp.cpp_standard >= 202002L);          // built as C++20+
    REQUIRE(fp.sample_precision == 32);            // Pulp sample type is float
    REQUIRE(std::strlen(fp.compiler) > 0);
    REQUIRE(std::strlen(fp.target) > 0);
    REQUIRE(std::strlen(fp.stdlib) > 0);
    REQUIRE(std::strlen(fp.abi_flags) > 0);
    REQUIRE(std::strlen(fp.sanitizers) > 0);       // "none" when no sanitizer
}

TEST_CASE("identical fingerprints match with no diff",
          "[hot-reload][fingerprint]") {
    auto a = current_build_fingerprint();
    auto b = current_build_fingerprint();
    REQUIRE(fingerprints_match(a, b));
    REQUIRE(fingerprint_diff(a, b).empty());
}

TEST_CASE("any ABI-relevant difference fails the gate and is reported",
          "[hot-reload][fingerprint]") {
    auto shell = current_build_fingerprint();

    SECTION("different compiler") {
        auto logic = shell;
        std::snprintf(logic.compiler, sizeof logic.compiler, "gcc 13.2.0");
        REQUIRE_FALSE(fingerprints_match(shell, logic));
        auto d = fingerprint_diff(shell, logic);
        REQUIRE(d.size() == 1);
        REQUIRE(d[0].rfind("compiler:", 0) == 0);
    }
    SECTION("different stdlib") {
        auto logic = shell;
        std::snprintf(logic.stdlib, sizeof logic.stdlib, "libstdc++ 20230801");
        REQUIRE_FALSE(fingerprints_match(shell, logic));
    }
    SECTION("NDEBUG / hardening flag drift (abi_flags)") {
        auto logic = shell;
        std::snprintf(logic.abi_flags, sizeof logic.abi_flags, "ndebug=0;exc=1;rtti=1");
        REQUIRE_FALSE(fingerprints_match(shell, logic));
    }
    SECTION("sanitizer mismatch") {
        auto logic = shell;
        std::snprintf(logic.sanitizers, sizeof logic.sanitizers, "tsan");
        REQUIRE_FALSE(fingerprints_match(shell, logic));
    }
    SECTION("sample precision mismatch (f32 vs f64)") {
        auto logic = shell;
        logic.sample_precision = 64;
        REQUIRE_FALSE(fingerprints_match(shell, logic));
    }
    SECTION("schema version mismatch") {
        auto logic = shell;
        logic.schema_version = shell.schema_version + 1;
        REQUIRE_FALSE(fingerprints_match(shell, logic));
    }
    SECTION("SDK version mismatch") {
        auto logic = shell;
        std::snprintf(logic.sdk_version, sizeof logic.sdk_version, "0.999.0");
        REQUIRE_FALSE(fingerprints_match(shell, logic));
    }
}

TEST_CASE("diff reports every differing field at once",
          "[hot-reload][fingerprint]") {
    auto shell = current_build_fingerprint();
    auto logic = shell;
    std::snprintf(logic.compiler, sizeof logic.compiler, "gcc 13.2.0");
    std::snprintf(logic.stdlib, sizeof logic.stdlib, "libstdc++ 20230801");
    logic.sample_precision = 64;
    auto d = fingerprint_diff(shell, logic);
    REQUIRE(d.size() == 3);
}
