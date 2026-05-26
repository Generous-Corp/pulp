// SPDX-License-Identifier: MIT
#pragma once

// Crash-isolated plugin scanner (macOS plan item 4.1).
//
// Loads a single plugin bundle in a CHILD PROCESS via the canonical
// `pulp::platform::ChildProcess` API + the pre-existing
// `pulp-scan-worker` helper binary (tools/scan-worker/). A crash, hang,
// or format error in the worker is reported back to the parent as a
// `ScanResult` with a structured `ScanStatus` instead of taking down
// the host.
//
// Builds on PR #2815 (ChildProcess) and workstream 03 slice 3.3b
// (`pulp-scan-worker` binary).
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

namespace pulp::host {

/// Outcome of an isolated scan attempt. Anything non-`Ok` means the
/// parent did NOT trust the worker's stdout — `descriptor` is only
/// meaningful when `status == Ok`.
enum class ScanStatus {
    Ok,           ///< Worker exited 0 and emitted a parseable descriptor.
    Crash,        ///< Worker terminated abnormally (signal / non-zero
                  ///< code with no JSON). The bundle is suspect — caller
                  ///< should blacklist it.
    Timeout,      ///< Worker exceeded the per-scan timeout. Treated as a
                  ///< soft crash for blacklist purposes.
    FormatError,  ///< Worker rejected the bundle (exit code 3 — extension
                  ///< not recognized). Not a crash; just unsupported.
    NotPlugin,    ///< Worker ran cleanly but emitted no descriptor (the
                  ///< file at `path` was not a plugin we can scan).
    WorkerMissing ///< The configured `worker_path` doesn't exist or
                  ///< can't be exec'd. Operational error, not a bundle
                  ///< problem.
};

struct ScanResult {
    ScanStatus status = ScanStatus::Crash;
    /// Populated only when `status == Ok`. The worker can produce more
    /// than one descriptor per bundle (CLAP factories can list several);
    /// the isolated scanner reports the first and the caller can re-scan
    /// for the full set in the same child if needed. Keep this simple
    /// for now — multi-descriptor bundles are rare in the wild and the
    /// host calls one-bundle-at-a-time.
    std::optional<PluginInfo> descriptor;
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
