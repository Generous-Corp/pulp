// validator_discovery.hpp — Discovery + healing for plugin-format validators
//
// `pulp validate` wraps three third-party tools — auval (Xcode),
// pluginval (Homebrew cask), and clap-validator (cargo install). One failure
// mode observed in the wild on 2026-04-24: a validator binary copied out of
// its `.app` bundle into `/usr/local/bin` retains a code signature that
// references peer files (`_CodeSignature/`, `Info.plist`) which don't exist
// outside the bundle. macOS amfid kills the process with SIGKILL (exit 137)
// before it can print anything, so an agent watching `pulp validate` sees a
// silent failure with no remediation hint.
//
// This module does the read-only discovery + an opt-in `--fix` heal pass.
// It is deliberately free of cli_common.hpp so the unit test binary
// (`pulp-test-cli-validator-discovery`) can compile just this TU + the test
// file without pulling pulp-cli's full runtime link surface in. Everything
// I/O-heavy (filesystem checks, ownership lookups, `spctl` invocation) is
// behind a `DiscoveryEnv` interface that tests stub out so the acceptance
// scenarios run deterministically on any host.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pulp::cli::validator_discovery {

namespace fs = std::filesystem;

// Each report represents one validator that we know how to look for.
// The same struct is rendered for every output mode (text, JSON-ish), and
// fed back into `apply_fixes()` when the user passes `--fix`.
enum class ValidatorStatus {
    Healthy,   // Found a usable copy. `path` is the chosen one.
    Broken,    // Found a copy, but it failed signature / smoke check.
               // `path` is the broken one; `reason` explains why.
    Missing,   // Not found at any of the priority paths. `install_hint`
               // tells the user how to get it.
};

// Per-tool ownership classification. Drives the safety boundary for `--fix`:
// user-owned breakage is healed in-place; root-owned breakage prints a sudo
// one-liner and never auto-elevates.
enum class PathOwnership {
    Unknown,        // path doesn't exist or stat() failed
    UserOwned,      // owned by the calling user (or in a user-writable dir)
    RootOrSystem,   // owned by root / system — `--fix` must escalate to user
};

struct ValidatorReport {
    std::string name;            // "auval" | "pluginval" | "clap-validator"
    std::string format;          // "AU" | "VST3" | "CLAP" — for messages

    ValidatorStatus status = ValidatorStatus::Missing;

    // Path that produced the status. Empty when status == Missing.
    fs::path path;

    // Human-readable why-this-status. For Broken this is the spctl
    // verdict line ("invalid resource directory…"); for Missing it's
    // a short "not installed" string; for Healthy it carries the
    // signing source ("signed, notarized") so reports can echo it.
    std::string reason;

    // Suggested remediation. For Broken this is the sudo / rm one-liner.
    // For Missing it's the install command. For Healthy it's empty.
    std::string remediation;

    // Where on disk the offending file lives (for Broken) — drives the
    // user-vs-root branch in `apply_fixes()`. For Missing/Healthy this
    // is `Unknown`.
    PathOwnership ownership = PathOwnership::Unknown;

    // True when status == Broken AND the path is user-owned, i.e.
    // safe for `--fix` to `unlink()` without sudo. Set during discovery
    // so the renderer can label the line "Auto-fixable." up front.
    bool fixable_without_sudo = false;
};

// Injectable I/O surface. Production builds wire this to real
// stat/spctl/PATH lookups; tests construct a hand-rolled instance with
// `path_exists` / `path_owner_uid` / `assess_signature` returning
// canned values so the 4 acceptance scenarios are deterministic.
//
// All callbacks must be safe to invoke from the doctor thread — no
// blocking on user input, no GUI prompts.
struct DiscoveryEnv {
    // Returns true iff the given path exists as a regular file or
    // executable bundle entry. The test stub returns true for paths
    // that the scenario simulates as present, false otherwise.
    std::function<bool(const fs::path&)> path_exists;

    // Returns true iff the given path is executable by the calling
    // user. Production wires this to `access(p, X_OK)`. Tests usually
    // mirror `path_exists` for simplicity.
    std::function<bool(const fs::path&)> path_executable;

    // Returns the uid of the file owner, or `std::numeric_limits<uint32_t>::max()`
    // on stat failure. Production reads `lstat(p).st_uid`. Tests
    // return the uid the scenario wants — 0 for root, anything else
    // for "user-owned".
    std::function<uint32_t(const fs::path&)> path_owner_uid;

    // The current user's uid. Production reads `getuid()`. Tests fix
    // this so ownership classification is deterministic across hosts.
    uint32_t current_uid = 0;

    // Returns true iff the given path passes the platform's signing
    // assessment. On macOS production wires this to a `spctl --assess
    // -v <path>` shellout that requires `accepted` in the output. On
    // non-Apple hosts production wires this to a `<tool> --version`
    // smoke-test that requires exit 0 + recognisable stdout.
    //
    // The second out-param receives the raw verdict line (e.g. the
    // `spctl` "rejected: invalid resource directory…" suffix) so the
    // report can quote it back to the user.
    std::function<bool(const fs::path& path,
                       const std::string& tool_name,
                       std::string& verdict_line)> assess_signature;

    // Looks up the absolute path that `command -v <name>` resolves to,
    // or empty if the tool isn't on PATH. Production uses the same
    // helper the rest of the CLI uses; tests return canned strings.
    std::function<fs::path(const std::string& tool_name)> resolve_in_path;

    // The user's home directory. Drives `~/.cargo/bin/clap-validator`
    // resolution. Tests inject a tmp path; production reads `$HOME`.
    fs::path home_dir;
};

// Build a `DiscoveryEnv` wired to real `stat` / `spctl` / PATH lookups.
// Defined in validator_discovery.cpp — production CLI uses this; tests
// construct their own DiscoveryEnv inline.
DiscoveryEnv make_default_env();

// Discover all known validators in the given environment. Returns one
// report per tool, in stable order: auval, pluginval, clap-validator.
// Discovery is fully read-only — no filesystem mutations, no shellouts
// outside what `env.assess_signature` chooses to do.
std::vector<ValidatorReport> discover_validators(const DiscoveryEnv& env);

// Apply the `--fix` heal pass. For each report:
//
//   - status == Broken && fixable_without_sudo:
//       attempt fs::remove(path); on success the report is mutated
//       to status=Missing with remediation = original install hint.
//       Counted in `auto_fixed`.
//
//   - status == Broken && !fixable_without_sudo:
//       no filesystem change. Counted in `needs_sudo`. The renderer
//       is expected to print the sudo one-liner verbatim.
//
//   - status == Missing:
//       no filesystem change. Counted in `still_missing` so the
//       renderer can include them in the post-fix summary.
//
//   - status == Healthy:
//       no-op.
//
// `dry_run` skips the actual `fs::remove()` while still updating the
// counters so `--fix --dry-run` previews what would happen.
struct FixOutcome {
    int auto_fixed = 0;
    int needs_sudo = 0;
    int still_missing = 0;
    int healthy = 0;
};

FixOutcome apply_fixes(std::vector<ValidatorReport>& reports, bool dry_run);

// Render the report set as a human-readable block. Returns the rendered
// text (so callers can write it to stdout/stderr or capture it for tests).
// Mirrors the validator-report format:
//
//   ✓ pluginval: /Applications/pluginval.app/... (signed, notarized)
//   ✗ pluginval: /usr/local/bin/pluginval — broken signature (...). Auto-fixable.
//   ⚠  clap-validator: not installed. Run `cargo install clap-validator`.
//
// `use_color` toggles ANSI color escapes; tests pass false so string
// matching stays simple.
std::string render_report(const std::vector<ValidatorReport>& reports,
                          bool use_color);

// Compute the exit code for `pulp doctor --validators` from the report
// set. Returns 0 iff every validator is Healthy; 1 otherwise. Missing
// counts as non-zero too — a host without auval/pluginval/clap-validator
// can't run `pulp validate` at all, so the doctor must surface it.
int compute_exit_code(const std::vector<ValidatorReport>& reports);

// True iff the report set contains at least one Broken entry. Used by
// the `pulp validate` preflight to decide whether to abort
// before launching the validator and risking the SIGKILL-by-amfid
// silent-fail mode.
bool has_broken_validator(const std::vector<ValidatorReport>& reports);

}  // namespace pulp::cli::validator_discovery
