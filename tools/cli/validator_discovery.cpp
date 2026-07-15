// validator_discovery.cpp — implementation, see header for the design notes.

#include "validator_discovery.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <sstream>

#if !defined(_WIN32)
#  include <sys/stat.h>
#  include <unistd.h>
#endif

// MSVC ships POSIX-style pipe helpers under the `_popen` / `_pclose`
// names; alias them so the cross-platform call sites below stay tidy.
#if defined(_WIN32)
#  define popen  _popen
#  define pclose _pclose
#endif

namespace pulp::cli::validator_discovery {

namespace {

// Per-validator priority list of well-known absolute install paths plus
// install-hint metadata. Discovery walks each list top-to-bottom and
// stops at the first path that exists. The "first existing" path also
// drives the Broken-vs-Healthy verdict — if the first existing copy is
// broken we surface that, even if a healthy one exists further down,
// because that's the binary the user's PATH typically resolves first
// (Homebrew puts /usr/local/bin ahead of /Applications cask shims).
//
// `priority` is small on purpose. Adding a fourth path? Update the
// validator_priority_paths() switch below + add a unit-test scenario.

struct ValidatorMeta {
    const char* name;
    const char* format;
    const char* install_hint;
};

constexpr std::array kValidators = {
    ValidatorMeta{"auval", "AU",
                  "ships with Xcode Command Line Tools "
                  "(`xcode-select --install`)"},
    ValidatorMeta{"pluginval", "VST3",
                  "brew install --cask pluginval (macOS) "
                  "| https://github.com/Tracktion/pluginval/releases"},
    ValidatorMeta{"clap-validator", "CLAP",
                  "cargo install clap-validator"},
};

// Build the priority list for one tool. Order matches the validator spec:
//   1. system binary path (e.g. /usr/bin/auval)
//   2. cask app-bundle binary (e.g. /Applications/pluginval.app/...)
//   3. PATH lookup (deferred to env.resolve_in_path so tests stay hermetic)
//   4. ~/.cargo/bin/<tool> for cargo-installed validators
//
// Returns absolute paths only — relative paths in the priority list
// would defeat the whole purpose of the diagnostic (the user's CWD
// shouldn't change which binary the doctor reports on).
std::vector<fs::path> validator_priority_paths(const std::string& tool,
                                               const DiscoveryEnv& env) {
    std::vector<fs::path> out;
    if (tool == "auval") {
        out.push_back("/usr/bin/auval");
        out.push_back("/System/Library/Frameworks/AudioToolbox.framework/"
                      "Versions/A/Resources/auval");
    } else if (tool == "pluginval") {
        // /usr/local/bin/pluginval is the historical Homebrew formula
        // path — it's also the path most affected by the rip-from-bundle
        // failure mode, so it stays first.
        out.push_back("/usr/local/bin/pluginval");
        out.push_back("/opt/homebrew/bin/pluginval");
        out.push_back("/Applications/pluginval.app/Contents/MacOS/pluginval");
    } else if (tool == "clap-validator") {
        if (!env.home_dir.empty()) {
            out.push_back(env.home_dir / ".cargo" / "bin" / "clap-validator");
        }
        out.push_back("/usr/local/bin/clap-validator");
        out.push_back("/opt/homebrew/bin/clap-validator");
    }

    // Append the PATH-resolved location last as a catch-all — covers
    // hosts that put validators in /opt/local/bin, ~/.local/bin, etc.
    if (env.resolve_in_path) {
        auto via_path = env.resolve_in_path(tool);
        if (!via_path.empty()) {
            // Only append if not already present so we don't double-test.
            if (std::find(out.begin(), out.end(), via_path) == out.end()) {
                out.push_back(via_path);
            }
        }
    }
    return out;
}

PathOwnership classify_ownership(const fs::path& p, const DiscoveryEnv& env) {
    if (!env.path_owner_uid) return PathOwnership::Unknown;
    auto uid = env.path_owner_uid(p);
    if (uid == std::numeric_limits<uint32_t>::max()) return PathOwnership::Unknown;
    if (uid == env.current_uid) return PathOwnership::UserOwned;
    return PathOwnership::RootOrSystem;
}

}  // namespace

// Production env wires real I/O. Kept thin — most logic lives in the
// pure-functional core above.
DiscoveryEnv make_default_env() {
    DiscoveryEnv env;

    env.path_exists = [](const fs::path& p) {
        std::error_code ec;
        return fs::exists(p, ec);
    };

    env.path_executable = [](const fs::path& p) {
#if defined(_WIN32)
        std::error_code ec;
        return fs::exists(p, ec);
#else
        return ::access(p.c_str(), X_OK) == 0;
#endif
    };

    env.path_owner_uid = [](const fs::path& p) -> uint32_t {
#if defined(_WIN32)
        (void)p;
        // Windows ownership model differs; treat everything as
        // user-owned so the safety boundary defaults to the more
        // forgiving branch. Discovery on Windows is currently
        // out of scope for this validator-discovery pass.
        return 0;
#else
        struct stat st{};
        if (::lstat(p.c_str(), &st) != 0) {
            return std::numeric_limits<uint32_t>::max();
        }
        return static_cast<uint32_t>(st.st_uid);
#endif
    };

#if defined(_WIN32)
    env.current_uid = 0;
#else
    env.current_uid = static_cast<uint32_t>(::getuid());
#endif

    env.assess_signature = [](const fs::path& path,
                              const std::string& tool_name,
                              std::string& verdict_line) -> bool {
        verdict_line.clear();
#if defined(__APPLE__)
        (void)tool_name;
        // The failure mode this diagnostic targets is:
        // a binary copied OUT of its .app bundle (`/usr/local/bin/pluginval`
        // copied from `/Applications/pluginval.app/Contents/MacOS/`)
        // retains a signature that references peer files inside the
        // bundle (`_CodeSignature/`, `Info.plist`). At launch time
        // amfid sees the signature claim fail and SIGKILLs the process
        // with exit 137 and zero stderr. The user has no diagnostic
        // they can act on without already knowing this exists.
        //
        // The naive `spctl --assess` check the spec proposes catches
        // that case but ALSO rejects:
        //   - system CLI tools like /usr/bin/auval ("the code is valid
        //     but does not seem to be an app") — actually fine to run
        //   - cargo-installed clap-validator (no Gatekeeper signature)
        //     — also fine to run, amfid doesn't intercept
        //
        // So we run codesign --verify first (which catches the
        // signature-integrity failure that drives amfid SIGKILL) and
        // only fall back to the spctl verdict if codesign passes. The
        // codesign verdicts we treat as Broken are exactly the ones
        // that imply "this binary will be killed at launch":
        //   - "invalid resource directory" / "directory or signature have been modified"
        //   - "invalid Info.plist" / "plist or signature have been modified"
        //   - "a sealed resource is missing or invalid"
        //   - "main executable failed strict validation"
        //
        // Anything else (unsigned, system tool, valid-but-not-an-app)
        // is treated as Healthy because it WILL run.

        auto shell_quote = [](const fs::path& p) {
            std::string s = "'";
            for (char c : p.string()) {
                if (c == '\'') s += "'\\''";
                else s += c;
            }
            s += "'";
            return s;
        };

        auto run_capture = [](const std::string& cmd, std::string& out, int& rc) {
            FILE* pipe = ::popen(cmd.c_str(), "r");
            if (!pipe) { rc = -1; return; }
            char buf[256];
            while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
            rc = ::pclose(pipe);
            while (!out.empty() &&
                   (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
                out.pop_back();
            }
        };

        // Step 1: codesign --verify is the authoritative integrity
        // check. If this passes, amfid will not kill the binary at
        // launch. If it fails, the failure mode is interesting only
        // when the failure pattern matches the "ripped from bundle"
        // signature drift — other failure patterns (unsigned cargo
        // binary) are not the bug we're hunting.
        std::string cs_out;
        int cs_rc = 0;
        run_capture("codesign --verify -v " + shell_quote(path) + " 2>&1",
                    cs_out, cs_rc);

        if (cs_rc == 0) {
            // Signature integrity is intact. Capture the spctl verdict
            // for display ("accepted / source=Notarized Developer ID")
            // but treat any successful codesign as Healthy regardless
            // of whether spctl accepted it (system CLI tools fail spctl
            // by design — they're not apps).
            std::string sp_out;
            int sp_rc = 0;
            run_capture("spctl --assess -v " + shell_quote(path) + " 2>&1",
                        sp_out, sp_rc);
            if (sp_rc == 0 && sp_out.find("accepted") != std::string::npos) {
                verdict_line = sp_out;
            } else {
                // Codesign passed but spctl rejected — common for
                // system CLI tools and cargo binaries. Surface a
                // friendly Healthy reason that says so.
                verdict_line = "valid signature on disk (Gatekeeper "
                               "policy not applicable to CLI binary)";
            }
            return true;
        }

        // Step 2: codesign failed. Decide whether this is the
        // "ripped from bundle" pattern (Broken, will SIGKILL) or
        // simply unsigned (Healthy, cargo install style).
        verdict_line = cs_out;
        // Patterns that indicate signature drift / integrity failure
        // — this is the amfid-kill class of failure.
        const char* broken_markers[] = {
            "invalid resource directory",
            "invalid Info.plist",
            "a sealed resource is missing",
            "directory or signature have been modified",
            "plist or signature have been modified",
            "main executable failed strict validation",
        };
        for (auto m : broken_markers) {
            if (cs_out.find(m) != std::string::npos) {
                return false;  // Broken
            }
        }
        // "code object is not signed at all" — true for cargo binaries
        // and other developer-built tools. They run fine.
        if (cs_out.find("not signed") != std::string::npos) {
            verdict_line = "unsigned (no Gatekeeper enforcement)";
            return true;
        }
        // Unknown codesign failure. Be conservative: don't flag as
        // Broken because we'd rather not block `pulp validate` on a
        // novel failure pattern. Treat as Healthy with the verdict
        // captured for the report.
        return true;
#else
        // Non-Apple smoke-test: `<tool> --version` exit 0 + recognisable
        // stdout. We use the tool name on stdin to keep the surface
        // identical for all three validators.
        std::string cmd = path.string() + " --version 2>&1";
        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe) return false;
        std::string captured;
        char buf[256];
        while (std::fgets(buf, sizeof(buf), pipe)) captured += buf;
        int rc = ::pclose(pipe);
        verdict_line = captured;
        // Be generous on the recognisable-stdout check — some tools
        // print "Pluginval x.y.z", others print just the version.
        return rc == 0 && (!captured.empty()) &&
               (captured.find(tool_name) != std::string::npos ||
                captured.find_first_of("0123456789") != std::string::npos);
#endif
    };

    env.resolve_in_path = [](const std::string& tool) -> fs::path {
#if defined(_WIN32)
        std::string cmd = "where " + tool + " 2>nul";
#else
        std::string cmd = "command -v '" + tool + "' 2>/dev/null";
#endif
        FILE* pipe = ::popen(cmd.c_str(), "r");
        if (!pipe) return {};
        char buf[1024];
        std::string out;
        while (std::fgets(buf, sizeof(buf), pipe)) out += buf;
        ::pclose(pipe);
        // Take the first line (where on Windows can list multiples).
        auto nl = out.find_first_of("\r\n");
        if (nl != std::string::npos) out.resize(nl);
        return out.empty() ? fs::path{} : fs::path(out);
    };

    if (const char* h = std::getenv("HOME")) {
        env.home_dir = fs::path(h);
    }

    return env;
}

std::vector<ValidatorReport> discover_validators(const DiscoveryEnv& env) {
    std::vector<ValidatorReport> out;
    out.reserve(kValidators.size());

    for (const auto& meta : kValidators) {
        ValidatorReport r;
        r.name = meta.name;
        r.format = meta.format;
        r.status = ValidatorStatus::Missing;
        r.reason = "not installed";
        r.remediation = meta.install_hint;

        auto paths = validator_priority_paths(meta.name, env);
        for (const auto& p : paths) {
            if (!env.path_exists || !env.path_exists(p)) continue;
            // Require the candidate to be executable before selecting
            // it. A stale non-exec
            // file at a high-priority location (e.g. someone touched
            // /usr/local/bin/pluginval to a zero-byte placeholder)
            // would otherwise mask runnable copies further down the
            // list and produce incorrect Healthy/Broken outcomes.
            // env.path_executable defaults to access(p, X_OK).
            if (env.path_executable && !env.path_executable(p)) continue;
            // Found a candidate. Validate it.
            std::string verdict;
            bool ok = env.assess_signature
                          ? env.assess_signature(p, meta.name, verdict)
                          : false;
            r.path = p;
            r.ownership = classify_ownership(p, env);

            if (ok) {
                r.status = ValidatorStatus::Healthy;
                // For macOS we echo the spctl verdict; for non-Apple we
                // echo the smoke-test stdout's first line.
                if (!verdict.empty()) {
                    auto nl = verdict.find('\n');
                    auto first = verdict.substr(0, nl);
                    // Common spctl line: "<path>: accepted\nsource=Notarized Developer ID"
                    // — keep the suffix for context if present.
                    r.reason = first;
                    if (nl != std::string::npos) {
                        auto rest = verdict.substr(nl + 1);
                        // Drop trailing whitespace / extra newlines
                        while (!rest.empty() &&
                               (rest.back() == '\n' || rest.back() == '\r')) {
                            rest.pop_back();
                        }
                        if (!rest.empty()) r.reason += " — " + rest;
                    }
                } else {
                    r.reason = "signed";
                }
                r.remediation.clear();
            } else {
                r.status = ValidatorStatus::Broken;
                r.reason = verdict.empty()
                    ? std::string("signature / smoke check failed")
                    : verdict;
                r.fixable_without_sudo = (r.ownership == PathOwnership::UserOwned);
                if (r.fixable_without_sudo) {
                    r.remediation = "rm " + p.string();
                } else {
                    r.remediation = "sudo rm " + p.string();
                }
            }
            // First-existing wins — the spec is explicit: that's the
            // copy the user's shell will dispatch, so that's the copy
            // we report on. Stop scanning further candidates.
            break;
        }
        out.push_back(std::move(r));
    }

    return out;
}

FixOutcome apply_fixes(std::vector<ValidatorReport>& reports, bool dry_run) {
    FixOutcome o;
    for (auto& r : reports) {
        switch (r.status) {
            case ValidatorStatus::Healthy:
                ++o.healthy;
                break;
            case ValidatorStatus::Missing:
                ++o.still_missing;
                break;
            case ValidatorStatus::Broken:
                if (r.fixable_without_sudo) {
                    if (!dry_run) {
                        std::error_code ec;
                        fs::remove(r.path, ec);
                        if (ec) {
                            // If fs::remove fails (e.g. permission flip
                            // mid-doctor, sticky bit, racing process)
                            // the broken binary is still on disk and
                            // `pulp validate` will still abort on it
                            // next run. Keep the report as Broken so
                            // the user sees the failure verbatim;
                            // bump `still_missing` so summaries
                            // distinguish "tried-and-failed" from
                            // "auto-fixed". Do NOT increment auto_fixed.
                            r.reason = "auto-fix failed: " + ec.message();
                            ++o.still_missing;
                            break;
                        }
                        // Successful removal: flip to Missing so the
                        // post-fix render shows the install hint as
                        // the next step.
                        r.status = ValidatorStatus::Missing;
                        r.reason = "broken copy removed; "
                                   "reinstall via the install hint";
                        for (const auto& m : kValidators) {
                            if (r.name == m.name) {
                                r.remediation = m.install_hint;
                                break;
                            }
                        }
                        r.path.clear();
                        r.fixable_without_sudo = false;
                    }
                    ++o.auto_fixed;
                } else {
                    ++o.needs_sudo;
                    // remediation already set to the sudo one-liner.
                }
                break;
        }
    }
    return o;
}

namespace {
// Tiny ANSI helpers — the production CLI has its own color layer in
// cli_common.hpp, but pulling that in would defeat the unit-test
// link isolation. Kept local + branchless for the off path.
const char* ok_color(bool on)    { return on ? "\033[32m" : ""; }
const char* fail_color(bool on)  { return on ? "\033[31m" : ""; }
const char* warn_color(bool on)  { return on ? "\033[33m" : ""; }
const char* dim_color(bool on)   { return on ? "\033[2m"  : ""; }
const char* reset(bool on)       { return on ? "\033[0m"  : ""; }
}  // namespace

std::string render_report(const std::vector<ValidatorReport>& reports,
                          bool use_color) {
    std::ostringstream os;
    for (const auto& r : reports) {
        switch (r.status) {
            case ValidatorStatus::Healthy:
                os << ok_color(use_color) << "  OK  " << reset(use_color)
                   << r.name << ": " << r.path.string()
                   << " (" << r.reason << ")\n";
                break;
            case ValidatorStatus::Broken:
                os << fail_color(use_color) << " FAIL " << reset(use_color)
                   << r.name << ": " << r.path.string()
                   << " — " << r.reason;
                if (r.fixable_without_sudo) {
                    os << ". Auto-fixable.";
                } else {
                    os << ". Root-owned — sudo required.";
                }
                os << "\n";
                if (!r.remediation.empty()) {
                    os << "        " << dim_color(use_color) << "fix: "
                       << reset(use_color) << r.remediation << "\n";
                }
                break;
            case ValidatorStatus::Missing:
                os << warn_color(use_color) << " WARN " << reset(use_color)
                   << r.name << ": not installed.";
                if (!r.remediation.empty()) {
                    os << " Run `" << r.remediation << "`.";
                }
                os << "\n";
                break;
        }
    }
    return os.str();
}

int compute_exit_code(const std::vector<ValidatorReport>& reports) {
    for (const auto& r : reports) {
        if (r.status != ValidatorStatus::Healthy) return 1;
    }
    return 0;
}

bool has_broken_validator(const std::vector<ValidatorReport>& reports) {
    for (const auto& r : reports) {
        if (r.status == ValidatorStatus::Broken) return true;
    }
    return false;
}

}  // namespace pulp::cli::validator_discovery
