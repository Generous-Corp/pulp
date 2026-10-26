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
#include <pulp/host/scan_blacklist.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/platform/child_process.hpp>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "support/unique_temp_dir.hpp"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
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
        path = pulp::test::make_unique_temp_dir(std::string("pulp-isolated-scanner-test-") + stem);
    }

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

class ScopedEnv {
public:
    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) previous_ = value;
    }

    ~ScopedEnv() {
        if (previous_)
            set(*previous_);
        else
            unset();
    }

    void set(const std::string& value) {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), value.c_str());
#else
        ::setenv(name_.c_str(), value.c_str(), 1);
#endif
    }

    void unset() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), "");
#else
        ::unsetenv(name_.c_str());
#endif
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
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

TEST_CASE("IsolatedPluginScanner never publishes an inspector session",
          "[host][isolated-scanner][inspect][negative]") {
    ScratchDir scratch("inspector-negative");
    const auto runtime_dir = scratch.path / "runtime";
    const auto bundle = scratch.path / "InspectorProbe.vst3";
    fs::create_directories(bundle / "Contents" / "Resources");
    ScopedEnv profile("PULP_INSPECT_PROFILE");
    ScopedEnv discovery("PULP_INSPECTOR_RUNTIME_DIR");
    profile.set("develop");
    discovery.set(runtime_dir.string());

    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_REAL_WORKER};
    const auto result = scanner.scan(bundle.string(), /*timeout_ms=*/5000);

    REQUIRE(result.status == ScanStatus::Ok);
    REQUIRE(result.descriptor.has_value());
    REQUIRE_FALSE(fs::exists(runtime_dir));
}

TEST_CASE("pulp-scan-worker command line reports usage and unsupported bundles",
          "[tools][scan-worker]") {
    auto usage = pulp::platform::exec(PULP_ISOLATED_SCANNER_REAL_WORKER, {}, 5000);
    REQUIRE(usage.exit_code == 2);
    REQUIRE_THAT(usage.stderr_output, ContainsSubstring("usage: pulp-scan-worker"));
    REQUIRE(usage.stdout_output.empty());

    ScratchDir scratch("worker-cli");
    auto unsupported = scratch.path / "Plugin.component";
    write_file(unsupported, "not a supported bundle");

    auto rejected = pulp::platform::exec(
        PULP_ISOLATED_SCANNER_REAL_WORKER,
        {unsupported.string()},
        5000);
    REQUIRE(rejected.exit_code == 3);
    REQUIRE_THAT(rejected.stderr_output, ContainsSubstring("unsupported bundle extension"));
    REQUIRE_THAT(rejected.stderr_output, ContainsSubstring(unsupported.string()));
    REQUIRE(rejected.stdout_output.empty());
}

// Regression coverage: when a directory contains multiple `.vst3` symlinks
// pointing at the same real bundle, the worker must preserve the requested-alias
// identity (lexical absolute path) instead of canonicalizing through symlinks.
// Otherwise a request for `alias.vst3` could emit a descriptor for `real.vst3`
// and `IsolatedPluginScanner::scan` (which consumes only the first JSON line)
// would return or blacklist a sibling.
TEST_CASE("pulp-scan-worker preserves requested-alias identity for symlinks",
          "[tools][scan-worker][issue-3110]") {
    ScratchDir scratch("symlink-alias");
    auto real_bundle = scratch.path / "Real.vst3";
    fs::create_directories(real_bundle / "Contents" / "Resources");
    auto alias_bundle = scratch.path / "Alias.vst3";

    std::error_code ec;
    fs::create_symlink(real_bundle, alias_bundle, ec);
    if (ec) {
        WARN("create_symlink not supported on this platform: " + ec.message());
        return;
    }

    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_REAL_WORKER};

    auto real_result = scanner.scan(real_bundle.string(), /*timeout_ms=*/5000);
    REQUIRE(real_result.status == ScanStatus::Ok);
    REQUIRE(real_result.descriptor.has_value());
    REQUIRE(real_result.descriptor->path == real_bundle.string());

    auto alias_result = scanner.scan(alias_bundle.string(), /*timeout_ms=*/5000);
    REQUIRE(alias_result.status == ScanStatus::Ok);
    REQUIRE(alias_result.descriptor.has_value());
    REQUIRE(alias_result.descriptor->path == alias_bundle.string());
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

TEST_CASE("IsolatedPluginScanner classifies unparseable worker output as ProtocolError",
          "[host][isolated-scanner][protocol]") {
    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_CRASH_HELPER};
    auto result = scanner.scan("garbage", /*timeout_ms=*/5000);

    // exit 0 + non-JSON stdout: nothing from it is trusted, and the worker,
    // not the bundle, is at fault.
    REQUIRE(result.status == ScanStatus::ProtocolError);
    REQUIRE_FALSE(result.descriptor.has_value());
    REQUIRE(result.descriptors.empty());
    REQUIRE_THAT(result.error_message, ContainsSubstring("not parseable"));
}

TEST_CASE("IsolatedPluginScanner classifies output past the capture cap as ProtocolError",
          "[host][isolated-scanner][protocol]") {
    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_CRASH_HELPER};
    auto result = scanner.scan("flood", /*timeout_ms=*/5000);

    REQUIRE(result.status == ScanStatus::ProtocolError);
    REQUIRE(result.descriptors.empty());
    REQUIRE_THAT(result.error_message, ContainsSubstring("capture limit"));
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

// ── PluginScanner routes CLAP enumeration through the worker ───────────

#ifdef PULP_SCAN_FIXTURE_PROBE_CLAP

namespace {

constexpr const char* kProbeId = "dev.pulp.test.scan-probe";

fs::path copy_fixture(const char* source, const fs::path& directory) {
    fs::create_directories(directory);
    const auto destination = directory / fs::path(source).filename();
    fs::copy_file(source, destination);
    return destination;
}

// Only the scratch directory, only CLAP, with the worker this tree built.
pulp::host::ScanOptions hermetic_clap_scan(const fs::path& directory,
                                           pulp::host::ScanBlacklist* blacklist) {
    pulp::host::ScanOptions options;
    options.scan_vst3 = false;
    options.scan_au = false;
    options.scan_lv2 = false;
    options.only_extra_paths = true;
    options.extra_paths.push_back(directory.string());
    options.blacklist = blacklist;
    options.scan_worker_path = PULP_ISOLATED_SCANNER_REAL_WORKER;
    return options;
}

} // namespace

TEST_CASE("PluginScanner isolates CLAP enumeration: an aborting bundle is blacklisted and the scan "
          "survives",
          "[host][isolated-scanner][scanner][blacklist]") {
    ScratchDir scratch("abort-bundle");
    const auto aborting = copy_fixture(PULP_SCAN_FIXTURE_ABORT_CLAP, scratch.path);
    copy_fixture(PULP_SCAN_FIXTURE_PROBE_CLAP, scratch.path);

    pulp::host::ScanBlacklist blacklist;
    const auto found =
        pulp::host::PluginScanner{}.scan(hermetic_clap_scan(scratch.path, &blacklist));

    REQUIRE(found.size() == 1);
    REQUIRE(found.front().unique_id == kProbeId);
    REQUIRE(blacklist.is_blacklisted(aborting.string()));
    REQUIRE_THAT(blacklist.get(aborting.string())->reason, ContainsSubstring("abnormally"));

    // The next scan skips the recorded bundle and still lists the good one.
    const auto again =
        pulp::host::PluginScanner{}.scan(hermetic_clap_scan(scratch.path, &blacklist));
    REQUIRE(again.size() == 1);
    REQUIRE(again.front().unique_id == kProbeId);
}

TEST_CASE("isolated and in-process CLAP scans enumerate the same PluginInfo",
          "[host][isolated-scanner][scanner]") {
    ScratchDir scratch("same-info");
    copy_fixture(PULP_SCAN_FIXTURE_PROBE_CLAP, scratch.path);

    const auto isolated =
        pulp::host::PluginScanner{}.scan(hermetic_clap_scan(scratch.path, nullptr));
    auto in_process_options = hermetic_clap_scan(scratch.path, nullptr);
    in_process_options.in_process_clap = true;
    const auto in_process = pulp::host::PluginScanner{}.scan(in_process_options);

    REQUIRE(isolated.size() == 1);
    REQUIRE(in_process.size() == 1);
    const auto& a = isolated.front();
    const auto& b = in_process.front();
    REQUIRE(a.unique_id == kProbeId);
    REQUIRE(a.name == b.name);
    REQUIRE(a.manufacturer == b.manufacturer);
    REQUIRE(a.version == b.version);
    REQUIRE(a.unique_id == b.unique_id);
    REQUIRE(a.path == b.path);
    REQUIRE(a.format == b.format);
    REQUIRE(a.is_instrument == b.is_instrument);
    REQUIRE(a.is_effect == b.is_effect);
    REQUIRE(a.num_inputs == b.num_inputs);
    REQUIRE(a.num_outputs == b.num_outputs);
    REQUIRE(a.category == b.category);
    REQUIRE(a.category == "Fx");
    REQUIRE(a.features == b.features);
    REQUIRE(a.features.size() == 2);
    REQUIRE(a.description == b.description);
    REQUIRE(a.description == "Scanner fixture with one descriptor");
    REQUIRE(a.has_editor == b.has_editor);
    REQUIRE(a.supports_sidechain == b.supports_sidechain);
    REQUIRE(a.supports_midi_in == b.supports_midi_in);
    REQUIRE(a.supports_midi_out == b.supports_midi_out);
}

TEST_CASE("find_scan_worker honours an explicit path, PULP_SCAN_WORKER, then the executable's tree",
          "[host][isolated-scanner][scanner]") {
    ScratchDir scratch("find-worker");
    const auto missing = (scratch.path / "no-such-worker").string();
    ScopedEnv env("PULP_SCAN_WORKER");
    env.unset();

    REQUIRE(pulp::host::find_scan_worker(PULP_ISOLATED_SCANNER_REAL_WORKER) ==
            PULP_ISOLATED_SCANNER_REAL_WORKER);
    // An explicit path that does not exist is a configuration error, not a
    // cue to look elsewhere.
    REQUIRE(pulp::host::find_scan_worker(missing).empty());

    env.set(PULP_ISOLATED_SCANNER_REAL_WORKER);
    REQUIRE(pulp::host::find_scan_worker() == PULP_ISOLATED_SCANNER_REAL_WORKER);
    env.set(missing);
    REQUIRE(pulp::host::find_scan_worker().empty());

    // This test binary sits in the build tree, so the worker is found by
    // walking up from it with nothing configured.
    env.unset();
    const auto discovered = pulp::host::find_scan_worker();
    REQUIRE_FALSE(discovered.empty());
    REQUIRE(fs::equivalent(discovered, PULP_ISOLATED_SCANNER_REAL_WORKER));
}

TEST_CASE("PluginScanner refuses in-process CLAP enumeration when no worker exists",
          "[host][isolated-scanner][scanner]") {
    ScratchDir scratch("no-worker");
    copy_fixture(PULP_SCAN_FIXTURE_PROBE_CLAP, scratch.path);
    ScopedEnv env("PULP_SCAN_WORKER");
    env.unset();

    auto options = hermetic_clap_scan(scratch.path, nullptr);
    options.scan_worker_path = (scratch.path / "no-such-worker").string();
    const auto report = pulp::host::PluginScanner{}.scan_report(options);

    // Nothing ran in this process, and the report says why and what to do.
    REQUIRE(report.plugins.empty());
    REQUIRE(report.clap_skipped_no_worker);
    REQUIRE_THAT(report.error, ContainsSubstring("no scan worker found"));
    REQUIRE_THAT(report.error, ContainsSubstring("--in-process"));

    // The explicit ask is what enables it.
    options.in_process_clap = true;
    const auto in_process = pulp::host::PluginScanner{}.scan_report(options);
    REQUIRE_FALSE(in_process.clap_skipped_no_worker);
    REQUIRE(in_process.plugins.size() == 1);
    REQUIRE(in_process.plugins.front().unique_id == kProbeId);
}

TEST_CASE("the worker keeps a plugin's stdout chatter out of the descriptor protocol",
          "[host][isolated-scanner][protocol]") {
    // The probe fixture prints a line to stdout from clap_entry.init. The
    // worker hands the plugin a stdout that is really stderr, so the parent
    // still reads clean descriptors.
    IsolatedPluginScanner scanner{PULP_ISOLATED_SCANNER_REAL_WORKER};
    const auto result = scanner.scan(PULP_SCAN_FIXTURE_PROBE_CLAP, /*timeout_ms=*/10000);

    REQUIRE(result.status == ScanStatus::Ok);
    REQUIRE(result.descriptors.size() == 1);
    REQUIRE(result.descriptors.front().unique_id == kProbeId);
}

#if !defined(_WIN32)
TEST_CASE("find_scan_worker refuses a group- or world-writable worker",
          "[host][isolated-scanner][scanner]") {
    ScratchDir scratch("writable-worker");
    const auto copy = scratch.path / "pulp-scan-worker";
    fs::copy_file(PULP_ISOLATED_SCANNER_REAL_WORKER, copy);

    fs::permissions(copy, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                              fs::perms::others_read | fs::perms::others_exec);
    REQUIRE(pulp::host::find_scan_worker(copy.string()) == copy.string());

    fs::permissions(copy, fs::perms::group_write, fs::perm_options::add);
    REQUIRE(pulp::host::find_scan_worker(copy.string()).empty());

    fs::permissions(copy, fs::perms::group_write, fs::perm_options::remove);
    fs::permissions(copy, fs::perms::others_write, fs::perm_options::add);
    REQUIRE(pulp::host::find_scan_worker(copy.string()).empty());

    // The same copy named through the environment is refused the same way.
    ScopedEnv env("PULP_SCAN_WORKER");
    env.set(copy.string());
    REQUIRE(pulp::host::find_scan_worker().empty());
}
#endif

#endif // PULP_SCAN_FIXTURE_PROBE_CLAP
