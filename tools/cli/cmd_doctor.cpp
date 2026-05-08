// cmd_doctor.cpp — pulp doctor command

#include "cli_common.hpp"
#include "fetchcontent_cache.hpp"
#include "projects_registry.hpp"
#include "validator_discovery.hpp"
#include "version_diag.hpp"

#include <algorithm>
#include <iostream>

namespace {

// Populate a ProjectEntry for a given root by reading the project's
// sdk_version (CMakeLists.txt for source-tree projects, pulp.toml
// otherwise) and cli_min_version. Mirrors the Slice 1 logic for the
// active project but applied to each registered/scanned entry.
// Issue #552.
pulp::cli::version_diag::ProjectEntry make_project_entry(
    const fs::path& root, const std::string& display_name,
    bool scanned)
{
    pulp::cli::version_diag::ProjectEntry e;
    e.path = root;
    e.name = display_name.empty() ? root.filename().string() : display_name;
    e.scanned = scanned;

    if (!fs::exists(root)) {
        e.missing_on_disk = true;
        return e;
    }

    // Prefer pulp.toml sdk_version, fall back to CMakeLists.txt VERSION
    // — matches the source-tree vs standalone split used elsewhere.
    std::string sdk_raw = read_pulp_toml_value(root, "sdk_version");
    if (sdk_raw.empty()) sdk_raw = read_project_cmake_version(root);
    e.sdk = pulp::cli::version_diag::parse_semver(sdk_raw);
    e.cli_min = pulp::cli::version_diag::read_project_cli_min_version(root);
    return e;
}

}  // namespace

int cmd_doctor(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto active_root = resolve_active_project_root(&standalone_mode);
    auto root = standalone_mode ? fs::path{} : active_root;

    // First positional argument selects a sub-mode for mobile dev-env
    // checks (#8 / #355). `pulp doctor` (no positional) keeps the
    // existing universal checks; `pulp doctor android` and
    // `pulp doctor ios` are the per-mobile-target lanes.
    std::string mode;  // empty = default
    bool fix_mode = false;
    bool ci_mode = false;
    bool dry_run = false;
    bool versions_mode = false;   // --versions: issue #499 Slice 1
    bool validators_mode = false; // --validators: issue #743
    bool scan_parents = false;    // --scan-parents: issue #552 Slice 1b
    bool caches_mode = false;     // --caches: issue #744
    bool json_mode = false;       // --json (works with --versions and --caches)
    for (auto& arg : args) {
        if (arg == "--fix") fix_mode = true;
        else if (arg == "--ci") ci_mode = true;
        else if (arg == "--dry-run") dry_run = true;
        else if (arg == "--versions") versions_mode = true;
        else if (arg == "--validators") validators_mode = true;
        else if (arg == "--scan-parents") scan_parents = true;
        else if (arg == "--caches") caches_mode = true;
        else if (arg == "--json") json_mode = true;
        else if (arg.rfind("--", 0) == 0) {
            std::cerr << "pulp doctor: unknown flag: " << arg << "\n";
            std::cerr << "Usage: pulp doctor [android|ios] [--fix] [--ci] [--dry-run] [--versions] [--validators] [--scan-parents] [--caches] [--json]\n";
            return 2;
        } else if (mode.empty()) {
            mode = arg;
        }
    }

    if (!mode.empty() && mode != "android" && mode != "ios") {
        std::cerr << "pulp doctor: unknown subcommand '" << mode << "'\n";
        std::cerr << "Usage: pulp doctor [android|ios] [--fix] [--ci] [--dry-run] [--versions] [--validators] [--scan-parents] [--caches] [--json]\n";
        return 2;
    }

    // `pulp doctor --caches` (issue #744) — discovery + healing for the
    // shared FetchContent cache (`~/Library/Caches/Pulp/fetchcontent-src`
    // on macOS, equivalent paths on Linux/Windows). Short-circuits the
    // rest of the doctor pipeline for the same reason --versions does:
    // the cache check has its own exit-code semantics (1 on any [!!]
    // entry) and mixing them would muddy the contract.
    if (caches_mode) {
        namespace fcc = pulp::cli::fetchcontent_cache;
        auto cache_root = fcc::default_cache_root();

        // Pull declared refs from the active project's CMakeLists.txt
        // (or the SDK's CMakeLists for source-tree mode). Stale-commit
        // detection only fires when we have something to compare
        // against; a missing CMakeLists is fine — entries fall through
        // to symlink/ownership-only classification.
        fcc::DeclaredRefs refs;
        if (!active_root.empty()) {
            refs = fcc::parse_declared_refs_from_file(
                active_root / "CMakeLists.txt");
        }
        auto env = fcc::make_real_env(cache_root, refs);
        auto entries = fcc::discover_fetchcontent_cache(env);

        if (json_mode) {
            // render_report_json itself always returns 0 (JSON is a
            // pure data surface), but automation needs a usable exit
            // code so it can detect unhealthy state without parsing
            // the JSON. Mirror the human-readable mode: exit non-zero
            // iff any entry is unhealthy. See Codex P2 review on
            // PR #753.
            (void)fcc::render_report_json(entries, cache_root, std::cout);
            return fcc::any_unhealthy(entries) ? 1 : 0;
        }

        int rc = fcc::render_report(entries, cache_root, std::cout);

        if (fix_mode && fcc::any_unhealthy(entries)) {
            std::cout << "\n";
            if (dry_run) {
                std::cout << "Dry-run — would remove fixable entries:\n";
            } else {
                std::cout << "Healing fixable entries...\n";
            }
            auto results = fcc::apply_fixes(entries, dry_run);
            int healed = 0, failed = 0;
            for (size_t i = 0; i < results.size(); ++i) {
                const auto& r = results[i];
                const auto& e = entries[i];
                if (r.outcome == fcc::FixOutcome::Skipped) continue;
                std::cout << "  " << fcc::fix_outcome_label(r.outcome)
                          << ": " << e.name;
                if (!r.error.empty()) std::cout << " (" << r.error << ")";
                std::cout << "\n";
                if (r.outcome == fcc::FixOutcome::Removed) ++healed;
                if (r.outcome == fcc::FixOutcome::Failed) ++failed;
            }
            std::cout << "\nHealed " << healed << " entr"
                      << (healed == 1 ? "y" : "ies");
            if (failed > 0) std::cout << ", " << failed << " failed";
            std::cout << ".\n";
            // Re-run discovery so the exit code reflects post-fix
            // state. A successful heal pass should drop us back to 0.
            if (!dry_run && healed > 0 && failed == 0) {
                auto env2 = fcc::make_real_env(cache_root, refs);
                auto entries2 = fcc::discover_fetchcontent_cache(env2);
                rc = fcc::any_unhealthy(entries2) ? 1 : 0;
            }
        }
        return rc;
    }

    // `pulp doctor --validators` (issue #743) — discover plugin-format
    // validators (auval / pluginval / clap-validator), surface broken
    // copies (signature-detached, ripped from a .app bundle, etc.) and
    // optionally heal user-owned breakage with `--fix`. Mirrors the
    // structure of `--versions` above: short-circuits the rest of the
    // doctor pipeline so script consumers can read its exit code in
    // isolation. Exits 0 only when every validator is Healthy.
    if (validators_mode) {
        namespace vd = pulp::cli::validator_discovery;
        auto env = vd::make_default_env();
        auto reports = vd::discover_validators(env);

        if (!ci_mode) {
            std::cout << color::bold() << "Pulp Doctor — Validators"
                      << color::reset() << "\n";
            std::cout << "===========\n\n";
        }

        // Render the pre-fix view first so the user can see what was
        // found before we mutate anything.
        std::cout << vd::render_report(reports, !ci_mode && g_color_enabled);

        if (fix_mode) {
            auto outcome = vd::apply_fixes(reports, dry_run);
            std::cout << "\n";
            auto plural = [](int n, const char* singular,
                             const char* plural) {
                return n == 1 ? singular : plural;
            };
            if (dry_run) {
                std::cout << color::dim() << "[dry-run] would auto-fix "
                          << outcome.auto_fixed << " broken user-owned "
                          << plural(outcome.auto_fixed, "copy", "copies")
                          << "\n" << color::reset();
            } else if (outcome.auto_fixed > 0) {
                std::cout << color::green() << "Auto-fixed "
                          << outcome.auto_fixed
                          << " broken user-owned validator "
                          << plural(outcome.auto_fixed, "copy", "copies")
                          << ".\n" << color::reset();
                // Re-render so the user sees the post-fix state.
                std::cout << "\n" << vd::render_report(
                    reports, !ci_mode && g_color_enabled);
            }
            if (outcome.needs_sudo > 0) {
                std::cout << color::yellow() << outcome.needs_sudo
                          << " broken root-owned validator "
                          << plural(outcome.needs_sudo, "copy", "copies")
                          << " require"
                          << (outcome.needs_sudo == 1 ? "s" : "")
                          << " sudo. Run the `fix:` lines above "
                             "manually.\n" << color::reset();
            }
        }

        return vd::compute_exit_code(reports);
    }

    // `pulp doctor --versions` is a dedicated diagnostic (issue #499
    // Slice 1). It short-circuits the rest of the doctor pipeline on
    // purpose — skew warnings are advisory and must not gate the
    // environment-readiness exit code, so mixing the two would just
    // confuse scripts that parse doctor's output.
    if (versions_mode) {
        using pulp::cli::version_diag::VersionReport;
        VersionReport report;
        report.cli = pulp::cli::version_diag::parse_semver(PULP_SDK_VERSION);

        // For plugin lookup we prefer the repo root (if we're inside one)
        // so developers hacking on the plugin see a matching version.
        fs::path plugin_json = pulp::cli::version_diag::locate_plugin_json(
            standalone_mode ? fs::path{} : active_root);
        report.plugin_json_path = plugin_json;
        report.plugin = pulp::cli::version_diag::read_plugin_version(plugin_json);
        // Slice 6 (#551): pick up the plugin's declared `min_cli_version`
        // so the diagnostic surfaces plugin ↔ CLI skew alongside the
        // existing project ↔ CLI checks.
        report.plugin_min_cli =
            pulp::cli::version_diag::read_plugin_min_cli_version(plugin_json);

        // Project SDK version. For standalone projects it lives in
        // `pulp.toml`; for source-tree use it lives in CMakeLists.txt.
        // Only populate when a project is genuinely present — we don't
        // want to parrot back the CLI's own baked-in version as "the
        // project's SDK" when no project exists.
        if (!active_root.empty()) {
            report.project_root = active_root;
            std::string project_sdk_raw;
            if (standalone_mode) {
                project_sdk_raw = read_pulp_toml_value(active_root, "sdk_version");
            } else {
                project_sdk_raw = read_project_cmake_version(active_root);
            }
            report.project_sdk = pulp::cli::version_diag::parse_semver(project_sdk_raw);
            report.project_cli_min =
                pulp::cli::version_diag::read_project_cli_min_version(active_root);
        }

        // Per-project registry (issue #552 Slice 1b). The registry is
        // authoritative; only `pulp projects add/remove` and
        // `pulp create` mutate it. We dedupe against the active
        // project so it isn't shown twice in the diagnostic.
        auto reg_path = pulp::cli::projects_registry::registry_path();
        auto registered = pulp::cli::projects_registry::read_registry(reg_path);
        std::vector<fs::path> seen_paths;
        if (!active_root.empty()) seen_paths.push_back(active_root);

        for (const auto& rp : registered) {
            if (std::find(seen_paths.begin(), seen_paths.end(), rp.path) !=
                seen_paths.end()) {
                continue;
            }
            report.projects.push_back(make_project_entry(rp.path, rp.name, false));
            seen_paths.push_back(rp.path);
        }

        // --scan-parents: opt-in ancestor walk. Matches happen deepest-
        // first (see projects_registry::scan_parent_pulp_projects). We
        // skip the active project so we don't duplicate it, and flag
        // every match as `scanned=true` so the report renders "(scanned)"
        // beside the name and the caller can tell it isn't registered.
        if (scan_parents) {
            auto cwd = fs::current_path();
            auto ancestors = pulp::cli::projects_registry::scan_parent_pulp_projects(cwd);
            for (const auto& a : ancestors) {
                if (std::find(seen_paths.begin(), seen_paths.end(), a) !=
                    seen_paths.end()) {
                    continue;
                }
                report.projects.push_back(make_project_entry(a, {}, true));
                seen_paths.push_back(a);
            }
        }

        // Always return 0 — skew findings are advisory. The rendered
        // "WARN" lines communicate the actionable information; gating
        // exit code on them would make this command unusable inside
        // scripts that gate on `pulp doctor` more broadly.
        if (json_mode) {
            (void)pulp::cli::version_diag::render_report_json(report);
        } else {
            (void)pulp::cli::version_diag::render_report(report);
        }
        return 0;
    }

    auto print_title = [&]() {
        if (mode == "android")    std::cout << "Pulp Doctor — Android";
        else if (mode == "ios")   std::cout << "Pulp Doctor — iOS";
        else                      std::cout << "Pulp Doctor";
    };

    if (!ci_mode) {
        std::cout << color::bold();
        print_title();
        std::cout << color::reset() << "\n";
        std::cout << "===========\n\n";
        if (mode.empty()) {
            if (standalone_mode && !active_root.empty()) {
                std::cout << color::dim() << "(SDK mode project — checking system tools for an installed SDK workflow)" << color::reset() << "\n\n";
            } else if (root.empty()) {
                std::cout << color::dim() << "(Not in a Pulp project — checking system tools only)" << color::reset() << "\n\n";
            } else {
                std::cout << color::dim() << "(Source-tree mode — checking the active Pulp checkout)" << color::reset() << "\n\n";
            }
        } else {
            std::cout << color::dim()
                      << "(mobile dev-env check — install hints follow each fail)"
                      << color::reset() << "\n\n";
        }
    } else {
        print_title();
        std::cout << "\n";
    }

    std::vector<DoctorCheck> checks;
    if (mode == "android")    checks = run_doctor_android_checks();
    else if (mode == "ios")   checks = run_doctor_ios_checks();
    else                      checks = run_doctor_checks(active_root, standalone_mode);

    int pass_count = 0, fail_count = 0, optional_skipped = 0;
    for (auto& c : checks) {
        if (c.passed) {
            ++pass_count;
            if (!ci_mode) {
                std::string msg = c.name;
                if (!c.detail.empty()) msg += " — " + c.detail;
                print_ok(msg);
            }
        } else {
            // Optional checks report advice but don't count toward
            // fail_count, so `pulp doctor android` returns 0 when
            // only optional accelerators (e.g. Google Android CLI,
            // #355) are missing. See #438 P1 for #389.
            if (c.optional) {
                ++optional_skipped;
                if (!ci_mode) {
                    std::string msg = c.name;
                    if (!c.detail.empty()) msg += " — " + c.detail;
                    print_warn(msg);
                    if (!c.fix.empty()) {
                        std::cout << "    Install (optional): "
                                  << color::yellow() << c.fix
                                  << color::reset() << "\n";
                    }
                }
                continue;
            }
            ++fail_count;
            if (ci_mode) {
                std::cerr << "FAIL: " << c.name;
                if (!c.detail.empty()) std::cerr << " — " << c.detail;
                if (!c.fix.empty()) std::cerr << " [fix: " << c.fix << "]";
                std::cerr << "\n";
            } else {
                std::string msg = c.name;
                if (!c.detail.empty()) msg += " — " + c.detail;
                print_fail(msg);
                if (!c.fix.empty()) {
                    if (fix_mode && !dry_run) {
                        std::cout << "    " << color::cyan() << "Fixing:" << color::reset() << " " << c.fix << "\n";
                        int rc = std::system(c.fix.c_str());
                        if (rc == 0) {
                            print_ok("Fixed");
                            --fail_count;
                            ++pass_count;
                        } else {
                            std::cout << "    Fix failed (exit " << rc << "). Run manually:\n";
                            std::cout << "      " << color::yellow() << c.fix << color::reset() << "\n";
                        }
                    } else if (dry_run) {
                        std::cout << "    " << color::dim() << "[dry-run] Would run: " << c.fix << color::reset() << "\n";
                    } else {
                        std::cout << "    Fix: " << color::yellow() << c.fix << color::reset() << "\n";
                    }
                }
            }
        }
    }

    if (!ci_mode) {
        std::cout << "\n  " << color::bold() << pass_count << "/" << (pass_count + fail_count)
                  << " checks passed" << color::reset();
        if (optional_skipped > 0) {
            std::cout << " (+" << optional_skipped << " optional skipped)";
        }
        if (fail_count > 0) {
            std::cout << " — run " << color::cyan() << "`pulp doctor --fix`" << color::reset() << " to resolve";
        }
        std::cout << "\n";
    }

    return fail_count > 0 ? 1 : 0;
}
