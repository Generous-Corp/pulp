// Unit tests for the ship-time PULP_TRACING guard (tools/cli/ship_tracing_guard.hpp).
//
// The guard refuses to sign/package a binary compiled with Perfetto tracing on.
// These tests exercise the scanner against fixture files — a full PULP_TRACING=ON
// build (which would embed the real sentinel) is not available in the default /
// coverage configuration, so the byte-level detection is proven here directly,
// and a sync check asserts the fixture sentinel still matches the runtime source.

#include "tools/cli/ship_tracing_guard.hpp"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

namespace {

fs::path unique_dir(const char* leaf) {
    // No Date/rand in the harness policy; the Catch2 test name + leaf is unique
    // enough per run, and we clean up in a RAII guard below.
    auto d = fs::temp_directory_path() / "pulp-ship-guard-test" / leaf;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

void write_file(const fs::path& p, const std::string& bytes) {
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

const std::string kSentinel{pulp::cli::kTracingShipSentinel};

}  // namespace

TEST_CASE("clean file has no tracing sentinel", "[ship][tracing]") {
    auto dir = unique_dir("clean");
    auto f = dir / "plugin.bin";
    write_file(f, std::string(50000, 'a') + "no tracing here" + std::string(50000, 'b'));
    REQUIRE_FALSE(pulp::cli::file_has_tracing_sentinel(f));
    REQUIRE(pulp::cli::artifact_tracing_offender(f).empty());
}

TEST_CASE("file with the sentinel is detected", "[ship][tracing]") {
    auto dir = unique_dir("dirty");
    auto f = dir / "traced.bin";
    write_file(f, "prefix " + kSentinel + " suffix");
    REQUIRE(pulp::cli::file_has_tracing_sentinel(f));
    REQUIRE(pulp::cli::artifact_tracing_offender(f) == f);
}

TEST_CASE("sentinel split across a chunk boundary is still detected",
          "[ship][tracing]") {
    // The scanner reads 64 KiB chunks with (needle-1) overlap. Place the sentinel
    // so it straddles the 64 KiB boundary — the case a naive per-chunk find misses.
    auto dir = unique_dir("boundary");
    auto f = dir / "boundary.bin";
    constexpr std::size_t kChunk = 1u << 16;
    const std::size_t pad = kChunk - (kSentinel.size() / 2);  // straddle the seam
    write_file(f, std::string(pad, 'x') + kSentinel + std::string(1000, 'y'));
    REQUIRE(pulp::cli::file_has_tracing_sentinel(f));
}

TEST_CASE("a bundle directory is scanned recursively for the sentinel",
          "[ship][tracing]") {
    auto dir = unique_dir("bundle");
    auto bundle = dir / "Plugin.vst3";
    auto macos = bundle / "Contents" / "MacOS";
    std::error_code ec;
    fs::create_directories(macos, ec);
    write_file(bundle / "Contents" / "Info.plist", "<plist/>");   // clean
    auto exe = macos / "Plugin";
    write_file(exe, std::string(200000, 'z') + kSentinel);        // traced Mach-O

    auto offender = pulp::cli::artifact_tracing_offender(bundle);
    REQUIRE_FALSE(offender.empty());
    REQUIRE(offender == exe);
}

TEST_CASE("a clean bundle directory reports no offender", "[ship][tracing]") {
    auto dir = unique_dir("clean-bundle");
    auto bundle = dir / "Clean.vst3" / "Contents" / "MacOS";
    std::error_code ec;
    fs::create_directories(bundle, ec);
    write_file(bundle / "Clean", std::string(100000, 'q'));
    REQUIRE(pulp::cli::artifact_tracing_offender(dir / "Clean.vst3").empty());
}

TEST_CASE("a missing path is not an error", "[ship][tracing]") {
    REQUIRE_FALSE(pulp::cli::file_has_tracing_sentinel("/nonexistent/pulp/xyz"));
    REQUIRE(pulp::cli::artifact_tracing_offender("/nonexistent/pulp/xyz").empty());
}

// Sync guard: the fixture sentinel must match the exact bytes the runtime emits.
// If someone edits core/runtime/src/trace.cpp's sentinel without updating the
// guard header, this fails loudly instead of silently disarming the ship guard.
TEST_CASE("guard sentinel matches the runtime source", "[ship][tracing]") {
    const fs::path src = fs::path(PULP_SOURCE_DIR) / "core/runtime/src/trace.cpp";
    REQUIRE(fs::exists(src));
    std::ifstream in(src, std::ios::binary);
    const std::string body((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    // The runtime declares `pulp_tracing_ship_sentinel[] = "<bytes>"`; the guard
    // header's kTracingShipSentinel must be exactly those bytes.
    REQUIRE(body.find(kSentinel) != std::string::npos);
    REQUIRE(body.find("pulp_tracing_ship_sentinel") != std::string::npos);
}
