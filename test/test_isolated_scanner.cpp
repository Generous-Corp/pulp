// SPDX-License-Identifier: MIT
//
// Crash-isolated plugin scanner (macOS plan item 4.1) — unit tests.
//
// Two binaries are exercised:
//   PULP_ISOLATED_SCANNER_REAL_WORKER  → the actual pulp-scan-worker
//     (validates the happy-path Ok / NotPlugin / FormatError flows).
//   PULP_ISOLATED_SCANNER_CRASH_HELPER → a tiny test helper that can
//     crash / hang / emit garbage on demand (validates the Crash,
//     Timeout, and "unparseable worker output" classifications).

#include <pulp/host/isolated_scanner.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#ifndef PULP_ISOLATED_SCANNER_REAL_WORKER
#error "PULP_ISOLATED_SCANNER_REAL_WORKER must point at the pulp-scan-worker binary"
#endif
#ifndef PULP_ISOLATED_SCANNER_CRASH_HELPER
#error "PULP_ISOLATED_SCANNER_CRASH_HELPER must point at the crash helper binary"
#endif

namespace fs = std::filesystem;
using Catch::Matchers::ContainsSubstring;
using pulp::host::IsolatedPluginScanner;
using pulp::host::ScanStatus;

namespace {

struct ScratchDir {
    fs::path path;

    explicit ScratchDir(const char* stem) {
        const auto counter =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path()
             / (std::string("pulp-isolated-scanner-test-") + stem + "-"
                + std::to_string(counter));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

void write_file(const fs::path& path, const std::string& body) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    REQUIRE(out.good());
    out << body;
}

}  // namespace

// ── Happy path: real worker, real bundle ───────────────────────────────

TEST_CASE("IsolatedPluginScanner returns Ok for a parseable VST3 bundle",
          "[host][isolated-scanner][item-4.1]") {
    ScratchDir scratch("ok-vst3");
    auto bundle = scratch.path / "Probe.vst3";
    fs::create_directories(bundle / "Contents" / "Resources");

    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_REAL_WORKER};
    auto result = scanner.scan(bundle.string(), /*timeout_ms=*/5000);

    REQUIRE(result.status == ScanStatus::Ok);
    REQUIRE(result.descriptor.has_value());
    REQUIRE(result.descriptor->name == "Probe");
    REQUIRE(result.descriptor->path == bundle.string());
    REQUIRE(result.descriptor->format == pulp::host::PluginFormat::VST3);
    REQUIRE(result.exit_code == 0);
}

TEST_CASE("IsolatedPluginScanner returns FormatError for an unknown extension",
          "[host][isolated-scanner][item-4.1]") {
    ScratchDir scratch("not-plugin");
    auto file = scratch.path / "random-file.txt";
    write_file(file, "this is not a plugin");

    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_REAL_WORKER};
    auto result = scanner.scan(file.string(), /*timeout_ms=*/5000);

    REQUIRE(result.status == ScanStatus::FormatError);
    REQUIRE(result.exit_code == 3);
    REQUIRE_THAT(result.error_message, ContainsSubstring("unsupported"));
}

// ── Crash path: deliberate-crash helper ────────────────────────────────

TEST_CASE("IsolatedPluginScanner classifies a worker SIGSEGV as Crash",
          "[host][isolated-scanner][item-4.1][crash]") {
    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_CRASH_HELPER};
    auto result = scanner.scan("crash", /*timeout_ms=*/5000);

    REQUIRE(result.status == ScanStatus::Crash);
    REQUIRE_FALSE(result.descriptor.has_value());
    // POSIX signal-kill → exit_code -1; Windows crash → large non-zero
    // exception code. Either way it's not 0.
    REQUIRE(result.exit_code != 0);
    REQUIRE_FALSE(result.error_message.empty());
}

TEST_CASE("IsolatedPluginScanner classifies a hung worker as Timeout",
          "[host][isolated-scanner][item-4.1][crash]") {
    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_CRASH_HELPER};
    auto result = scanner.scan("timeout", /*timeout_ms=*/250);

    REQUIRE(result.status == ScanStatus::Timeout);
    REQUIRE_FALSE(result.descriptor.has_value());
    REQUIRE_THAT(result.error_message, ContainsSubstring("timed out"));
}

TEST_CASE("IsolatedPluginScanner classifies unparseable worker output as Crash",
          "[host][isolated-scanner][item-4.1][crash]") {
    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_CRASH_HELPER};
    auto result = scanner.scan("garbage", /*timeout_ms=*/5000);

    // exit 0 + non-JSON stdout — we don't trust that and report Crash.
    REQUIRE(result.status == ScanStatus::Crash);
    REQUIRE_FALSE(result.descriptor.has_value());
    REQUIRE_THAT(result.error_message, ContainsSubstring("not parseable"));
}

TEST_CASE("IsolatedPluginScanner returns NotPlugin when worker exits clean with no output",
          "[host][isolated-scanner][item-4.1]") {
    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_CRASH_HELPER};
    auto result = scanner.scan("silent", /*timeout_ms=*/5000);

    REQUIRE(result.status == ScanStatus::NotPlugin);
    REQUIRE_FALSE(result.descriptor.has_value());
    REQUIRE(result.exit_code == 0);
}

// ── Operational error path ─────────────────────────────────────────────

TEST_CASE("IsolatedPluginScanner reports WorkerMissing when worker_path is absent",
          "[host][isolated-scanner][item-4.1]") {
    ScratchDir scratch("missing");
    auto missing = scratch.path / "no-such-worker";

    IsolatedPluginScanner scanner{missing.string()};
    auto result = scanner.scan("any-path", /*timeout_ms=*/1000);

    REQUIRE(result.status == ScanStatus::WorkerMissing);
    REQUIRE_THAT(result.error_message, ContainsSubstring("not found"));
}
