// SPDX-License-Identifier: MIT
#pragma once

// Crash-isolated plugin scanner.
//
// Loads a single plugin bundle in a CHILD PROCESS via the canonical
// `pulp::platform::ChildProcess` API + the pre-existing
// `pulp-scan-worker` helper binary (tools/scan-worker/). A crash, hang,
// or format error in the worker is reported back to the parent as a
// `ScanResult` with a structured `ScanStatus` instead of taking down
// the host.
//
// Uses the `pulp-scan-worker` binary.
//
// Usage:
//   pulp::host::IsolatedPluginScanner s{worker_path};
//   auto result = s.scan(bundle_path, /*timeout_ms=*/5000);
//   if (result.status == ScanStatus::Ok) {
//       use(result.descriptor);
//   } else if (result.status == ScanStatus::Crash) {
//       blacklist.blacklist(bundle_path, result.error_message);
//   }

#include <pulp/host/scanner.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::host {

/// Locate the `pulp-scan-worker` binary for isolated scanning. Order:
/// `explicit_path` when non-empty, the PULP_SCAN_WORKER environment variable,
/// then `pulp-scan-worker` beside the running executable or under `bin/` or
/// `tools/scan-worker/` in that directory and up to four parents, which
/// covers an installed SDK (`bin/pulp` next to `bin/pulp-scan-worker`), a
/// build tree (`tools/cli/pulp-cpp` and `tools/scan-worker/pulp-scan-worker`),
/// and test binaries under `build/test/`. The walk stops at an application
/// bundle (`.app`) and at the first directory that holds a `bin/`, so it never
/// reaches `/Applications` or another install prefix's siblings. A candidate
/// that is group- or world-writable is refused with a warning, since anyone
/// who can write it can run code in every scan. Returns an empty string when
/// no candidate qualifies; an explicit or environment path is returned only
/// when it exists and qualifies.
std::string find_scan_worker(std::string_view explicit_path = {});

/// Outcome of an isolated scan attempt. Anything non-`Ok` means the
/// parent did NOT trust the worker's stdout — `descriptor` is only
/// meaningful when `status == Ok`.
enum class ScanStatus {
    Ok,            ///< Worker exited 0 and emitted a parseable descriptor.
    Crash,         ///< Worker terminated abnormally (signal / non-zero
                   ///< code with no JSON). The bundle is suspect — caller
                   ///< should blacklist it.
    Timeout,       ///< Worker exceeded the per-scan timeout. Treated as a
                   ///< soft crash for blacklist purposes.
    FormatError,   ///< Worker rejected the bundle (exit code 3 — extension
                   ///< not recognized). Not a crash; just unsupported.
    NotPlugin,     ///< Worker ran cleanly but emitted no descriptor (the
                   ///< file at `path` was not a plugin we can scan).
    WorkerMissing, ///< The configured `worker_path` doesn't exist or
                   ///< can't be exec'd. Operational error, not a bundle
                   ///< problem.
    ProtocolError  ///< The worker exited 0 but its stdout could not be
                   ///< trusted: it exceeded the capture limit or a line
                   ///< did not parse. The worker owns that channel (the
                   ///< plugin's stdout is redirected to stderr before it
                   ///< loads), so this is a worker or transport fault,
                   ///< not the bundle's, and callers must not blacklist.
};

struct ScanResult {
    ScanStatus status = ScanStatus::Crash;
    /// The first descriptor, populated only when `status == Ok`. Kept for
    /// callers that key the bundle by one identity (blacklist, cache).
    std::optional<PluginInfo> descriptor;
    /// Every descriptor the worker emitted, in bundle order, populated only
    /// when `status == Ok`. A CLAP factory can list several plugins per
    /// bundle; this is what the directory scanner consumes so an isolated
    /// scan enumerates the same set an in-process scan would.
    std::vector<PluginInfo> descriptors;
    /// Worker exit status (raw OS exit code, or -1 if it never started /
    /// was signal-killed). Useful for diagnostics in the result log.
    int exit_code = -1;
    /// Human-readable error explanation (stderr tail, "timeout", or
    /// "worker not found"). Stored verbatim so the parent can write it
    /// straight into `ScanBlacklist::blacklist(..., reason)`.
    std::string error_message;
};

class IsolatedPluginScanner {
public:
    /// Construct with the path to the `pulp-scan-worker` binary that
    /// will be exec'd for each scan. Resolution policy is intentionally
    /// caller-controlled — Pulp ships the worker next to the CLI at
    /// install time, but tests inject `$<TARGET_FILE:pulp-scan-worker>`
    /// directly, and an embedded host may keep it elsewhere.
    explicit IsolatedPluginScanner(std::string worker_path)
        : worker_path_(std::move(worker_path)) {}

    /// Scan a single bundle in a child process. Always returns; never
    /// propagates a crash from the worker back into the caller.
    ScanResult scan(const std::string& bundle_path, int timeout_ms = 5000) const;

    /// Accessor for the resolved worker path (mostly for diagnostics).
    const std::string& worker_path() const noexcept { return worker_path_; }

private:
    std::string worker_path_;
};

}  // namespace pulp::host
