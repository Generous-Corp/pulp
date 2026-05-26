// mac_runtime_validators.hpp — macOS runtime validators for `pulp validate`
//
// Phase 5 of the Linux→macOS cross-build chainer plan
// (planning/2026-05-24-linux-macos-chainer-gap-closure-plan.md).
// These validators run on Apple Silicon to confirm that artifacts
// produced by the Linux-hosted cross lane will actually load and run
// before they are published as part of a private release. They are
// invoked via `pulp validate --target <standalone|auv3|macho>`.
//
// All three validators are pure-logic + shell-out: they accept a
// bundle path, run a focused check, and return a structured result.
// The `MacValidatorEnv` indirection lets the unit tests substitute
// hermetic stubs for `find_executable_in_path`, `exec_output`, and
// `run`, so we never need real `.app`/`.appex`/`auval` on the test
// host to assert dispatch + error-string contracts.
//
// The validators are intentionally *additive* to the existing
// per-format flow in `cmd_validate.cpp`: they are only reached when
// the user passes `--target`. Default `pulp validate` keeps its
// current behaviour byte-for-byte.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace pulp::cli::mac_runtime {

namespace fs = std::filesystem;

// A single validator result. `tool` is the underlying CLI we shelled
// out to (e.g. "auval", "codesign", "open"). `status` is one of
// "pass", "fail", "skip". `exit_code` is the shell-out exit code (or
// -1 when the validator skipped before running anything). `summary`
// is a truncated stderr/stdout excerpt for the human report; the JSON
// renderer reuses it as the "error" field for "fail" / "skip" rows.
struct ValidatorResult {
    std::string target;       // "standalone" / "auv3" / "macho"
    std::string tool;         // shell-out command name
    std::string bundle;       // absolute bundle path
    std::string status;       // "pass" / "fail" / "skip"
    int exit_code{-1};
    std::string summary;
};

// Dependency injection seam — every shell-out goes through here so
// tests can substitute deterministic stubs. Defaults bind to the
// real implementations in cli_common.{hpp,cpp}.
struct MacValidatorEnv {
    // Look up an executable by basename in $PATH; empty string means
    // not found. Defaults to `find_executable_in_path`.
    std::function<std::string(const std::string&)> find_executable;
    // Run a command, capture combined stdout+stderr, return (exit, output).
    // Defaults wrap `exec_output` (output) + `run` (exit) so tests can
    // observe both. The default impl runs the command twice; production
    // code only cares about the second copy when failure occurred, so
    // the perf hit is acceptable for an audit-time validator.
    std::function<std::pair<int, std::string>(const std::string&)> run_capture;
    // Check that a file exists. Defaults to `fs::exists`.
    std::function<bool(const fs::path&)> path_exists;
    // Compile-time platform flag. Real builds always pass `true` on
    // Apple. Tests on non-Apple hosts force `true` to exercise the
    // dispatch table.
    bool is_apple_host{true};
};

MacValidatorEnv make_default_env();

// Validators. Each returns a single ValidatorResult; callers
// aggregate results across multiple bundles themselves.

ValidatorResult run_standalone_validator(const fs::path& bundle,
                                         const MacValidatorEnv& env);
ValidatorResult run_auv3_validator(const fs::path& bundle,
                                   const MacValidatorEnv& env);
ValidatorResult run_macho_validator(const fs::path& bundle,
                                    const MacValidatorEnv& env);

// Convenience: `pulp validate --target all <bundle>` runs all three
// and returns them in dispatch order. Callers can decide failure
// policy (any-fail vs all-fail).
std::vector<ValidatorResult> run_all_targets(const fs::path& bundle,
                                             const MacValidatorEnv& env);

// Parse `--target <name>` into the matching set of dispatch entries.
// Returns empty when `name` is unrecognised; callers must validate
// before invoking. Accepts: "standalone", "auv3", "macho", "all".
std::vector<std::string> expand_target_name(const std::string& name);

// Pure-logic helpers exported for unit-testing.

// Resolve `<bundle>.app/Contents/MacOS/<binary>` for a standalone bundle.
// Returns empty when the bundle isn't a `.app`, or no MacOS dir, or
// no executable inside. Pure path arithmetic — no fs IO besides
// `env.path_exists`.
fs::path resolve_standalone_executable(const fs::path& bundle,
                                       const MacValidatorEnv& env);

// Extract (type, subt, manu) from an AUv3 .appex Info.plist. The plist
// is read via `plutil -p` (so we don't need a plist parser in the CLI).
// Returns empty strings on parse failure.
struct AuComponentTuple {
    std::string type;
    std::string subtype;
    std::string manufacturer;
};
AuComponentTuple parse_au_component_tuple(const std::string& plutil_output);

// Walk a bundle's `Contents/MacOS` + `Contents/Frameworks` and return
// every executable / dylib path. Used by the Mach-O validator to
// iterate inputs for `codesign --verify` and the LC_VERSION_MIN check.
std::vector<fs::path> enumerate_mach_o_payloads(const fs::path& bundle,
                                                const MacValidatorEnv& env);

// Parse `otool -l <path>` output and return the minimum macOS version
// declared by LC_VERSION_MIN_MACOSX or LC_BUILD_VERSION. Returns
// empty string when no version load command is present. The string
// is the raw "13.0" / "12.3.1" form.
std::string extract_min_macos_version(const std::string& otool_output);

// Compare a parsed "13.0" / "12.3.1" version against the AudioWorkgroup
// floor of macOS 13.0. Returns true when the version meets the floor;
// returns true (permissive) when the version string is empty so we don't
// fail a bundle that simply lacks the load command (printed as a warn).
bool meets_macos_floor(const std::string& version);

}  // namespace pulp::cli::mac_runtime
