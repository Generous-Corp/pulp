// cmd_project_bump.cpp — `pulp project pin` / `pulp project bump`
// (roadmap item P11-1).
//
// The command surface wraps the pure-logic core in
// project_bump.{hpp,cpp} and fans it out across per-project and
// `--all` flows:
//
//   pulp project bump                     Bump CWD project to CLI's own version
//   pulp project bump 0.28.0              Bump to explicit version (positional)
//   pulp project bump --to=0.28.0         Same as above (named flag)
//   pulp project bump --all               Iterate ~/.pulp/projects.json
//   pulp project bump --all --to=0.28.0   All projects to explicit version
//   pulp project bump --dry-run           Show plan without writing
//   pulp project bump --force-dirty       Skip the git-clean gate
//   pulp project bump --allow-downgrade   Allow bumping to an older version
//   pulp project bump --verify-builds     Build each project post-bump; rollback on failure
//
// Every successful `bump` writes `~/.pulp/bump-undo-<timestamp>.json`
// so a mistaken bump is always one command away from recovery.
// Migration notes (Slice 3 #548) surface after a successful bump so
// users see "pulp_add_au_v3 was renamed to pulp_add_auv3" (or
// whatever the hop introduced) inline with the bump report.

#include "cmd_project_internal.hpp"
#include "migration_index.hpp"
#include "projects_registry.hpp"

#include <cstddef>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace pb = pulp::cli::project_bump;
namespace prjreg = pulp::cli::projects_registry;
namespace mig = pulp::cli::migration;

namespace pulp_cli::project_detail {

namespace {

// ── Per-project bump ────────────────────────────────────────────────────────
//
// Returns an UndoEntry describing the outcome. Callers treat entries
// with status == "bumped" as successful; everything else is surfaced
// in the report. The rewritten CMakeLists.txt is persisted here on
// success (unless dry_run is true), and the old pin is captured in
// the entry so the batch-level undo file can be written by the
// caller after the whole iteration is done.

pb::UndoEntry bump_one(const fs::path& project_path,
                       const std::string& target_version,
                       const BumpOptions& opts,
                       const std::string& caller_name_hint) {
    pb::UndoEntry entry;
    entry.project_path = project_path;
    entry.project_name = caller_name_hint.empty()
                             ? project_path.filename().string()
                             : caller_name_hint;
    entry.status = "skipped";

    if (!fs::exists(project_path)) {
        entry.status = "failed";
        entry.failure_reason = "project path does not exist";
        return entry;
    }

    if (is_pulp_source_root(project_path)) {
        entry.status = "skipped";
        entry.failure_reason = "this is the Pulp source checkout; use Pulp's release/version workflow, not `pulp project bump`";
        return entry;
    }

    const bool standalone = is_standalone_project(project_path);
    auto cmake_path = project_path / "CMakeLists.txt";
    if (!fs::exists(cmake_path)) {
        entry.status = "failed";
        entry.failure_reason = "no CMakeLists.txt in project";
        return entry;
    }

    // Git-clean gate.
    if (!opts.force_dirty && pin_files_are_dirty(project_path, standalone)) {
        entry.status = "skipped";
        entry.failure_reason = standalone
            ? "CMakeLists.txt or pulp.toml has uncommitted changes (use --force-dirty or commit/stash first)"
            : "CMakeLists.txt has uncommitted changes (use --force-dirty or commit/stash first)";
        return entry;
    }

    std::map<fs::path, std::string> staged;
    std::map<fs::path, std::string> originals;

    auto add_edit = [&](const fs::path& path,
                        pb::PinKind kind,
                        const std::string& old_value,
                        bool old_has_v,
                        const std::string& new_value) -> bool {
        pb::UndoEdit edit;
        edit.path = path;
        edit.kind = kind;
        edit.old_value = old_value;
        edit.new_value = new_value;
        edit.old_value_style_has_v = old_has_v;
        if (!stage_edit(staged, edit, new_value)) return false;
        entry.edits.push_back(std::move(edit));
        return true;
    };

    std::string current;

    if (standalone) {
        auto toml_path = project_path / "pulp.toml";
        auto toml = read_text(toml_path);
        if (toml.empty()) {
            entry.status = "failed";
            entry.failure_reason = "pulp.toml is empty or unreadable";
            return entry;
        }

        auto sdk_site = pb::find_toml_string_value(toml, "sdk_version",
                                                   pb::PinKind::PulpTomlSdkVersion);
        entry.pin_kind = sdk_site.kind;
        entry.old_pin = sdk_site.current_pin;
        entry.old_pin_style_has_v = false;

        current = pb::normalize_pin(sdk_site.current_pin);
        if (current.empty()) {
            entry.status = "skipped";
            entry.failure_reason = "pulp.toml sdk_version doesn't parse as semver";
            return entry;
        }

        if (!opts.allow_downgrade && pb::is_downgrade(current, target_version)) {
            entry.status = "skipped";
            entry.failure_reason = "target version older than current SDK pin (use --allow-downgrade to override)";
            return entry;
        }

        if (current != target_version && !opts.allow_redundant) {
            if (auto main_pin = main_pinned_version_at_origin(project_path, true);
                main_pin && !pb::is_downgrade(*main_pin, target_version)) {
                entry.status = "skipped";
                entry.failure_reason = "origin/main already pins SDK " + *main_pin +
                    " >= target " + target_version +
                    " (rebase first or use --allow-redundant)";
                return entry;
            }
        }

        if (current != target_version) {
            if (!add_edit(toml_path, pb::PinKind::PulpTomlSdkVersion,
                          sdk_site.current_pin, false, target_version)) {
                entry.status = "failed";
                entry.failure_reason = "could not stage pulp.toml sdk_version rewrite";
                return entry;
            }
        }

        auto cmake = read_text(cmake_path);
        if (cmake.empty()) {
            entry.status = "failed";
            entry.failure_reason = "CMakeLists.txt is empty or unreadable";
            return entry;
        }
        auto find_site = pb::find_find_package_pulp_version(cmake);
        auto find_current = pb::normalize_pin(find_site.current_pin);
        if (!find_current.empty() && find_current != target_version) {
            if (!add_edit(cmake_path, pb::PinKind::CMakeFindPackagePulpVersion,
                          find_site.current_pin, false, target_version)) {
                entry.status = "failed";
                entry.failure_reason = "could not stage find_package(Pulp ...) rewrite";
                return entry;
            }
        }

        auto path_site = pb::find_toml_string_value(toml, "sdk_path",
                                                    pb::PinKind::PulpTomlSdkPath);
        if (!path_site.current_pin.empty()) {
            auto replacement = managed_sdk_path_replacement(fs::path(path_site.current_pin),
                                                            current,
                                                            target_version);
            if (replacement && *replacement != path_site.current_pin) {
                if (!add_edit(toml_path, pb::PinKind::PulpTomlSdkPath,
                              path_site.current_pin, false, *replacement)) {
                    entry.status = "failed";
                    entry.failure_reason = "could not stage managed sdk_path rewrite";
                    return entry;
                }
            } else if (current != target_version) {
                entry.notes.push_back("custom sdk_path left unchanged; `pulp build` will verify it");
            }
        }
    } else {
        auto source = read_text(cmake_path);
        if (source.empty()) {
            entry.status = "failed";
            entry.failure_reason = "CMakeLists.txt is empty or unreadable";
            return entry;
        }

        auto site = pb::find_pin_site(source);
        entry.pin_kind = site.kind;
        entry.old_pin = site.current_pin;
        entry.old_pin_style_has_v = pb::pin_has_v_prefix(site.current_pin);

        if (site.kind == pb::PinKind::Unknown) {
            entry.status = "skipped";
            entry.failure_reason = "no recognizable Pulp pin (FetchContent_Declare / pulp_add_project / project VERSION)";
            return entry;
        }

        if (pb::refuse_dynamic_pin(site)) {
            entry.status = "skipped";
            entry.failure_reason = "dynamic pin (branch / SHA) — leave alone";
            return entry;
        }

        current = pb::normalize_pin(site.current_pin);
        if (current.empty()) {
            entry.status = "skipped";
            entry.failure_reason = "current pin doesn't parse as semver";
            return entry;
        }

        if (!opts.allow_downgrade && pb::is_downgrade(current, target_version)) {
            entry.status = "skipped";
            entry.failure_reason = "target version older than current pin (use --allow-downgrade to override)";
            return entry;
        }

        if (!opts.allow_redundant) {
            if (auto main_pin = main_pinned_version_at_origin(project_path, false);
                main_pin && !pb::is_downgrade(*main_pin, target_version)) {
                entry.status = "skipped";
                entry.failure_reason = "origin/main already pins SDK " + *main_pin +
                    " >= target " + target_version +
                    " (rebase first or use --allow-redundant)";
                return entry;
            }
        }

        if (current != target_version) {
            if (!add_edit(cmake_path, site.kind, site.current_pin,
                          entry.old_pin_style_has_v, target_version)) {
                entry.status = "failed";
                entry.failure_reason = "pin rewrite failed (source drifted between scan and write)";
                return entry;
            }
        }
    }

    if (entry.edits.empty()) {
        entry.status = "skipped";
        entry.failure_reason = "already at target version";
        return entry;
    }

    if (opts.dry_run) {
        entry.status = "dry_run";
        return entry;
    }

    for (const auto& [path, body] : staged) {
        originals[path] = read_text(path);
    }

    std::vector<fs::path> written;
    for (const auto& [path, body] : staged) {
        if (!write_text_atomic(path, body)) {
            for (const auto& wrote : written) {
                (void)write_text_atomic(wrote, originals[wrote]);
            }
            entry.status = "failed";
            entry.failure_reason = "could not write pin file (permissions?)";
            return entry;
        }
        written.push_back(path);
    }

    entry.status = "bumped";

    if (opts.verify_builds) {
        // Minimal verify: run `cmake -S <proj> -B <proj>/build-bump-verify`
        // and a build. If either fails, roll back.
        //
        // This intentionally uses a throwaway build dir so the real
        // `build/` stays untouched. The CLI doesn't try to honor a
        // user-specified build dir here — that's a follow-up.
        auto verify_build = project_path / "build-bump-verify";
        std::string cfg = "cmake -S " + shell_quote(project_path) +
                          " -B " + shell_quote(verify_build) +
                          " -DCMAKE_BUILD_TYPE=Debug";
        if (standalone) {
            auto sdk = resolve_standalone_sdk(project_path, true);
            if (sdk.resolved_sdk_dir.empty()) {
                for (const auto& [path, body] : originals) {
                    (void)write_text_atomic(path, body);
                }
                entry.status = "failed";
                entry.failure_reason = "build verification failed — could not obtain target SDK, pin rolled back";
                return entry;
            }
            cfg += " -DCMAKE_PREFIX_PATH=" + shell_quote(sdk.resolved_sdk_dir);
        }
        cfg += output_to_null();
        int rc = run(cfg);
        if (rc == 0) {
            std::string bld = "cmake --build " + shell_quote(verify_build) + output_to_null();
            rc = run(bld);
        }
        if (rc != 0) {
            // Roll back.
            for (const auto& [path, body] : originals) {
                (void)write_text_atomic(path, body);
            }
            std::error_code ec;
            fs::remove_all(verify_build, ec);
            entry.status = "failed";
            entry.failure_reason = "build verification failed — pin rolled back";
            return entry;
        }
        std::error_code ec;
        fs::remove_all(verify_build, ec);
    }

    return entry;
}

// ── Report printing ─────────────────────────────────────────────────────────

void print_report(const pb::UndoBatch& batch, bool dry_run) {
    int bumped = 0, dry = 0, skipped = 0, failed = 0;
    for (const auto& e : batch.entries) {
        if      (e.status == "bumped")  ++bumped;
        else if (e.status == "dry_run") ++dry;
        else if (e.status == "skipped") ++skipped;
        else if (e.status == "failed")  ++failed;
    }

    std::cout << "\n"
              << color::bold()
              << (dry_run ? "pulp project bump (dry run) "
                          : "pulp project bump ")
              << "target=" << batch.target_version << color::reset() << "\n";

    for (const auto& e : batch.entries) {
        std::string marker;
        std::string color_code = color::dim();
        if (e.status == "bumped") {
            marker = "bumped ";
            color_code = color::green();
        } else if (e.status == "dry_run") {
            marker = "would bump ";
            color_code = color::cyan();
        } else if (e.status == "skipped") {
            marker = "skipped ";
            color_code = color::yellow();
        } else if (e.status == "failed") {
            marker = "failed ";
            color_code = color::red();
        }
        std::cout << "  " << color_code << marker << color::reset()
                  << e.project_name;
        if (!e.old_pin.empty()) {
            std::cout << "  " << color::dim()
                      << fmt_pin(e.old_pin, e.old_pin_style_has_v)
                      << " -> " << batch.target_version
                      << color::reset();
        }
        if (!e.failure_reason.empty()) {
            std::cout << "\n      " << color::dim() << e.failure_reason << color::reset();
        }
        for (const auto& note : e.notes) {
            std::cout << "\n      " << color::dim() << note << color::reset();
        }
        std::cout << "\n      " << color::dim() << e.project_path.string() << color::reset() << "\n";
    }

    std::cout << "\nSummary: "
              << bumped  << " bumped, "
              << dry     << " would-bump, "
              << skipped << " skipped, "
              << failed  << " failed\n";
}

// ── Migration notes (Slice 3 integration) ───────────────────────────────────
//
// After the batch lands, surface any applicable migration notes for
// the highest "from" version we bumped (older end of each hop).
// Users want to see "here are the things that changed between your
// old pin and the new one" — the range is installed_version → target_version
// per project, but we unify on the minimum old-pin in the batch to
// cover the widest set of notes.
void print_migration_notes(const pb::UndoBatch& batch) {
    std::string from;
    for (const auto& e : batch.entries) {
        if (e.status != "bumped" && e.status != "dry_run") continue;
        auto norm = pb::normalize_pin(e.old_pin);
        if (norm.empty()) continue;
        if (from.empty() || pb::is_downgrade(from, norm)) from = norm;
    }
    if (from.empty() || batch.target_version.empty()) return;
    auto entries = mig::applicable_entries(from, batch.target_version);
    if (entries.empty()) {
        std::cout << "\nNo migration notes apply for " << from
                  << " -> " << batch.target_version << ".\n";
        return;
    }
    std::cout << "\n"
              << color::bold() << "Migration notes " << from
              << " -> " << batch.target_version << color::reset() << "\n\n";
    std::cout << mig::render_notes_text(entries, from, batch.target_version);
}

}  // namespace

// ── Dispatch: bump ──────────────────────────────────────────────────────────

int do_bump(const std::vector<std::string>& args) {
    bool want_help = false;
    auto opts = parse_bump_options(args, want_help);
    if (want_help) { print_bump_help(); return 0; }
    if (!opts.error.empty()) {
        std::cerr << opts.error << "\n";
        return 2;
    }

    // --to wins over positional, but accept positional as a legacy
    // alias. Positional sanity: at most one token.
    if (opts.positional.size() > 1) {
        std::cerr << "pulp project bump: unexpected extra version argument '"
                  << opts.positional[1] << "'\n";
        return 2;
    }
    if (opts.to_version.empty() && !opts.positional.empty()) {
        opts.to_version = opts.positional.front();
    }
    if (opts.to_version.empty()) {
        opts.to_version = PULP_SDK_VERSION;
    }

    // Validate target semver.
    auto triple = pb::parse_semver_strict(opts.to_version);
    if (!triple.ok) {
        std::cerr << "pulp project bump: invalid target version '"
                  << opts.to_version << "' (expected X.Y.Z)\n";
        return 1;
    }
    opts.to_version = std::to_string(triple.major) + "." +
                      std::to_string(triple.minor) + "." +
                      std::to_string(triple.patch);

    auto cli_triple = pb::parse_semver_strict(PULP_SDK_VERSION);
    if (!opts.allow_cli_skew &&
        cli_triple.ok &&
        pb::compare_semver(triple, cli_triple) > 0) {
        std::cerr << "pulp project bump: target SDK v" << opts.to_version
                  << " is newer than this pulp CLI v" << PULP_SDK_VERSION
                  << ". Run `pulp upgrade " << opts.to_version
                  << "` first, or pass --allow-cli-skew for an unsupported bump.\n";
        return 1;
    }

    // Resolve the list of projects we're operating on.
    std::vector<fs::path> targets;
    std::vector<std::string> names;

    if (opts.all) {
        auto reg = prjreg::registry_path();
        auto projects = prjreg::read_registry(reg);
        if (projects.empty()) {
            std::cerr << "pulp project bump --all: registry is empty (run `pulp projects add` first)\n";
            return 1;
        }
        for (const auto& p : projects) {
            targets.push_back(p.path);
            names.push_back(p.name.empty() ? p.path.filename().string() : p.name);
        }
    } else {
        bool found_pulp_source = false;
        auto target = find_bumpable_project_root_from(fs::current_path(), &found_pulp_source);
        if (found_pulp_source) {
            std::cerr << "pulp project bump: refusing to run inside the Pulp source checkout.\n"
                      << "This command bumps consumer project SDK pins; use `pulp version bump` "
                      << "and the release workflow for Pulp itself.\n";
            return 1;
        }
        if (target.empty()) {
            std::cerr << "pulp project bump: not inside a bumpable Pulp project "
                      << "(expected pulp.toml or a CMakeLists.txt with a Pulp pin)\n";
            return 1;
        }
        targets.push_back(target);
        names.push_back(target.filename().string());
    }

    // Run each.
    pb::UndoBatch batch;
    batch.timestamp = pb::now_iso8601_utc();
    batch.target_version = opts.to_version;
    for (std::size_t i = 0; i < targets.size(); ++i) {
        batch.entries.push_back(bump_one(targets[i], opts.to_version, opts, names[i]));
    }

    print_report(batch, opts.dry_run);

    // Write undo file only when we actually changed something.
    bool any_bumped = false;
    for (const auto& e : batch.entries) if (e.status == "bumped") { any_bumped = true; break; }

    if (any_bumped && !opts.dry_run) {
        auto home = pulp_home();
        if (!home.empty()) {
            auto undo_path = pb::undo_batch_path(home, batch.timestamp);
            if (!pb::write_undo_batch(undo_path, batch)) {
                std::cerr << "Warning: could not write undo file at " << undo_path.string() << "\n";
            } else {
                std::cout << "\nUndo file: " << undo_path.string() << "\n";
                std::cout << "  Run `pulp project undo` to revert.\n";
            }
        }
        print_migration_notes(batch);
    } else if (opts.dry_run) {
        print_migration_notes(batch);
    }

    // Exit code policy: if --all, keep 0 even when some entries
    // failed (partial failure is isolated by design). Single-project
    // mode reports non-zero on failure so scripts can detect it.
    if (!opts.all) {
        for (const auto& e : batch.entries) {
            if (e.status == "failed") return 1;
            if (e.status == "skipped") return 2;  // distinguishable from failure
        }
    }
    return 0;
}

}  // namespace pulp_cli::project_detail
