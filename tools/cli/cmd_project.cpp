// cmd_project.cpp — `pulp project bump` / `pulp project undo`.
//
// Release-discovery Slice 7 (#564 / parent #499). The command surface
// wraps the pure-logic core in project_bump.{hpp,cpp} and fans it out
// across per-project and `--all` flows:
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
//   pulp project undo                     Revert the newest bump batch
//   pulp project undo <timestamp>         Revert a specific batch
//
// Every successful `bump` writes `~/.pulp/bump-undo-<timestamp>.json`
// so a mistaken bump is always one command away from recovery.
// Migration notes (Slice 3 #548) surface after a successful bump so
// users see "pulp_add_au_v3 was renamed to pulp_add_auv3" (or
// whatever the hop introduced) inline with the bump report.

#include "cli_common.hpp"
#include "migration_index.hpp"
#include "project_bump.hpp"
#include "projects_registry.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace pb = pulp::cli::project_bump;
namespace prjreg = pulp::cli::projects_registry;
namespace mig = pulp::cli::migration;

namespace {

// ── Options ─────────────────────────────────────────────────────────────────

struct BumpOptions {
    std::string to_version;          // may be empty → default to CLI version
    bool all = false;
    bool dry_run = false;
    bool force_dirty = false;
    bool allow_downgrade = false;
    bool verify_builds = false;      // opt-in, also honors update.verify_builds
    std::vector<std::string> positional;
};

std::string strip_eq_value(const std::string& arg, const std::string& flag) {
    // Returns the value portion of --flag=value, or empty if no '='.
    auto pos = arg.find('=');
    if (pos == std::string::npos) return {};
    if (arg.substr(0, pos) != flag) return {};
    return arg.substr(pos + 1);
}

BumpOptions parse_bump_options(const std::vector<std::string>& args,
                               bool& out_help) {
    BumpOptions opts;
    out_help = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--help" || a == "-h" || a == "help") { out_help = true; continue; }
        if (a == "--all")              { opts.all = true; continue; }
        if (a == "--dry-run")          { opts.dry_run = true; continue; }
        if (a == "--force-dirty")      { opts.force_dirty = true; continue; }
        if (a == "--allow-downgrade")  { opts.allow_downgrade = true; continue; }
        if (a == "--verify-builds")    { opts.verify_builds = true; continue; }
        if (a == "--to") {
            if (i + 1 < args.size()) opts.to_version = args[++i];
            continue;
        }
        if (auto v = strip_eq_value(a, "--to"); !v.empty()) {
            opts.to_version = v;
            continue;
        }
        if (!a.empty() && a.front() != '-') {
            opts.positional.push_back(a);
            continue;
        }
        // Unknown flag — keep going but surface later. Slice 7 doesn't
        // do strict unknown-flag rejection the way cmd_validate does;
        // that's tracked under issue #520.
        std::cerr << "pulp project bump: ignoring unknown argument '" << a << "'\n";
    }
    return opts;
}

// ── Help text ───────────────────────────────────────────────────────────────

void print_project_help() {
    std::cout <<
        "pulp project — manage a Pulp project's pinned SDK version\n\n"
        "Usage:\n"
        "  pulp project bump [<version>] [--to=X] [--all] [--dry-run]\n"
        "                    [--force-dirty] [--allow-downgrade]\n"
        "                    [--verify-builds]\n"
        "  pulp project undo [<timestamp>]\n"
        "\n"
        "Run `pulp project bump --help` or `pulp project undo --help`\n"
        "for command-specific details.\n";
}

void print_bump_help() {
    std::cout <<
        "pulp project bump — update the pinned Pulp SDK version\n\n"
        "Usage:\n"
        "  pulp project bump                     Bump CWD to the CLI's own version\n"
        "  pulp project bump <version>           Bump CWD to <version> (positional)\n"
        "  pulp project bump --to=<version>      Bump CWD to <version> (named)\n"
        "  pulp project bump --all               Iterate ~/.pulp/projects.json\n"
        "  pulp project bump --all --to=<version>\n"
        "\n"
        "Flags:\n"
        "  --dry-run            Show the plan without rewriting anything\n"
        "  --force-dirty        Skip the git-clean gate (risky — changes mingle)\n"
        "  --allow-downgrade    Permit target older than current pin\n"
        "  --verify-builds      Build each project post-bump; roll back on failure\n"
        "\n"
        "Every successful bump writes ~/.pulp/bump-undo-<timestamp>.json so\n"
        "`pulp project undo` can revert. Migration notes from Slice 3 (#548)\n"
        "print after the report.\n";
}

void print_undo_help() {
    std::cout <<
        "pulp project undo — revert a previous `pulp project bump`\n\n"
        "Usage:\n"
        "  pulp project undo              Revert the newest batch\n"
        "  pulp project undo <timestamp>  Revert a specific batch (e.g. 2026-04-21T14-30-00Z)\n"
        "\n"
        "Lists candidate batches under ~/.pulp/bump-undo-*.json.\n";
}

// ── Helpers ─────────────────────────────────────────────────────────────────

std::string read_text(const fs::path& p) {
    if (!fs::exists(p)) return {};
    std::ifstream f(p);
    if (!f.is_open()) return {};
    std::ostringstream os;
    os << f.rdbuf();
    return os.str();
}

bool write_text_atomic(const fs::path& path, const std::string& body) {
    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << body;
        if (!f.good()) return false;
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        fs::rename(tmp, path, ec);
        if (ec) {
            std::error_code ec2;
            fs::remove(tmp, ec2);
            return false;
        }
    }
    return true;
}

// Check if CMakeLists.txt inside `project_path` has uncommitted
// changes according to `git status --porcelain`. Returns true when
// the file is modified, false when clean OR when git is not available
// (we refuse to block the user on a missing-tool false negative).
bool cmake_is_dirty(const fs::path& project_path) {
    std::error_code ec;
    if (!fs::is_directory(project_path / ".git", ec) &&
        !fs::exists(project_path / ".git", ec)) {
        // Not a git repo — nothing to gate against.
        return false;
    }
    // Quote the project path for the shell. Use "-C <path>" so we
    // don't have to cd.
    std::string cmd = "git -C " + shell_quote(project_path) +
                      " status --porcelain -- CMakeLists.txt 2>/dev/null";
    auto out = trim(exec_output(cmd));
    return !out.empty();
}

// Pretty-print a pin with optional `v` prefix preserved. `raw` may
// or may not already carry a leading 'v' — the `has_v` flag records
// the ORIGINAL shape we captured; we always normalize before
// formatting so the output reads "v0.23.0" not "vv0.23.0".
std::string fmt_pin(const std::string& raw, bool has_v) {
    if (raw.empty()) return "(none)";
    auto bare = pb::normalize_pin(raw);
    if (bare.empty()) bare = raw;  // fall back to whatever was passed in
    return has_v ? ("v" + bare) : bare;
}

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

    auto cmake_path = project_path / "CMakeLists.txt";
    if (!fs::exists(cmake_path)) {
        entry.status = "failed";
        entry.failure_reason = "no CMakeLists.txt in project";
        return entry;
    }

    // Git-clean gate.
    if (!opts.force_dirty && cmake_is_dirty(project_path)) {
        entry.status = "skipped";
        entry.failure_reason = "CMakeLists.txt has uncommitted changes (use --force-dirty or commit/stash first)";
        return entry;
    }

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

    auto current = pb::normalize_pin(site.current_pin);
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

    if (current == target_version) {
        entry.status = "skipped";
        entry.failure_reason = "already at target version";
        return entry;
    }

    auto new_source = pb::rewrite_pin(source, site, target_version,
                                       entry.old_pin_style_has_v);
    if (!new_source) {
        entry.status = "failed";
        entry.failure_reason = "pin rewrite failed (source drifted between scan and write)";
        return entry;
    }

    if (opts.dry_run) {
        entry.status = "dry_run";
        return entry;
    }

    if (!write_text_atomic(cmake_path, *new_source)) {
        entry.status = "failed";
        entry.failure_reason = "could not write CMakeLists.txt (permissions?)";
        return entry;
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
                          " -DCMAKE_BUILD_TYPE=Debug >/dev/null 2>&1";
        int rc = run(cfg);
        if (rc == 0) {
            std::string bld = "cmake --build " + shell_quote(verify_build) + " >/dev/null 2>&1";
            rc = run(bld);
        }
        if (rc != 0) {
            // Roll back.
            (void)write_text_atomic(cmake_path, source);
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

// ── Dispatch: bump ──────────────────────────────────────────────────────────

int do_bump(const std::vector<std::string>& args) {
    bool want_help = false;
    auto opts = parse_bump_options(args, want_help);
    if (want_help) { print_bump_help(); return 0; }

    // --to wins over positional, but accept positional as a legacy
    // alias. Positional sanity: at most one token.
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
        auto cwd = fs::current_path();
        targets.push_back(cwd);
        names.push_back(cwd.filename().string());
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

// ── Dispatch: undo ──────────────────────────────────────────────────────────

int do_undo(const std::vector<std::string>& args) {
    if (!args.empty() && (args[0] == "--help" || args[0] == "-h" || args[0] == "help")) {
        print_undo_help();
        return 0;
    }

    auto home = pulp_home();
    if (home.empty()) {
        std::cerr << "pulp project undo: could not determine pulp home (HOME / USERPROFILE unset)\n";
        return 1;
    }

    fs::path target;
    if (!args.empty()) {
        auto stamp = args[0];
        target = pb::undo_batch_path(home, stamp);
        if (!fs::exists(target)) {
            std::cerr << "pulp project undo: no batch at " << target.string() << "\n";
            return 1;
        }
    } else {
        auto batches = pb::list_undo_batches(home);
        if (batches.empty()) {
            std::cerr << "pulp project undo: no bump batches on disk under " << home.string() << "\n";
            return 1;
        }
        target = batches.front();
    }

    auto batch = pb::read_undo_batch(target);
    if (!batch) {
        std::cerr << "pulp project undo: could not parse " << target.string() << "\n";
        return 1;
    }

    std::cout << color::bold() << "Reverting bump batch "
              << batch->timestamp << " (target was "
              << batch->target_version << ")" << color::reset() << "\n";

    int reverted = 0, skipped = 0, failed = 0;
    for (const auto& e : batch->entries) {
        if (e.status != "bumped") {
            ++skipped;
            continue;
        }
        auto cmake_path = e.project_path / "CMakeLists.txt";
        if (!fs::exists(cmake_path)) {
            std::cerr << "  " << color::yellow() << "missing" << color::reset()
                      << " " << e.project_name << "  (" << cmake_path.string() << ")\n";
            ++failed;
            continue;
        }
        auto source = read_text(cmake_path);
        auto site = pb::find_pin_site(source);
        if (site.kind != e.pin_kind) {
            std::cerr << "  " << color::yellow() << "skipped" << color::reset()
                      << " " << e.project_name
                      << "  (pin kind changed since bump)\n";
            ++skipped;
            continue;
        }
        auto current = pb::normalize_pin(site.current_pin);
        if (current != batch->target_version) {
            std::cerr << "  " << color::yellow() << "skipped" << color::reset()
                      << " " << e.project_name
                      << "  (current pin " << current
                      << " is not the target " << batch->target_version << ")\n";
            ++skipped;
            continue;
        }
        auto restored = pb::rewrite_pin(source, site,
                                         pb::normalize_pin(e.old_pin),
                                         e.old_pin_style_has_v);
        if (!restored || !write_text_atomic(cmake_path, *restored)) {
            std::cerr << "  " << color::red() << "failed" << color::reset()
                      << " " << e.project_name << "  (rewrite or write failed)\n";
            ++failed;
            continue;
        }
        std::cout << "  " << color::green() << "reverted" << color::reset()
                  << " " << e.project_name
                  << "  " << batch->target_version
                  << " -> " << fmt_pin(e.old_pin, e.old_pin_style_has_v) << "\n";
        ++reverted;
    }

    std::cout << "\nSummary: " << reverted << " reverted, "
              << skipped << " skipped, " << failed << " failed\n";

    if (failed == 0) {
        std::error_code ec;
        fs::remove(target, ec);
        std::cout << "Removed undo file " << target.string() << "\n";
    } else {
        std::cout << "Undo file retained (" << target.string()
                  << ") — inspect failures and retry.\n";
    }
    return failed == 0 ? 0 : 1;
}

}  // namespace

int cmd_project(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_project_help();
        return args.empty() ? 1 : 0;
    }

    const auto& sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "bump") return do_bump(rest);
    if (sub == "undo") return do_undo(rest);

    std::cerr << "pulp project: unknown subcommand '" << sub << "'\n";
    print_project_help();
    return 2;
}
