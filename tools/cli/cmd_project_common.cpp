// cmd_project_common.cpp — shared helpers for the `pulp project`
// sub-command translation units (roadmap item P11-1).
//
// Extracted byte-identical from the original cmd_project.cpp: option
// parsing, help text, and the pin-file / project-root helpers that
// `do_bump`, `do_undo`, and `do_unpin` all depend on.

#include "cmd_project_internal.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

namespace pb = pulp::cli::project_bump;

namespace pulp_cli::project_detail {

namespace {

std::string strip_eq_value(const std::string& arg, const std::string& flag) {
    // Returns the value portion of --flag=value, or empty if no '='.
    auto pos = arg.find('=');
    if (pos == std::string::npos) return {};
    if (arg.substr(0, pos) != flag) return {};
    return arg.substr(pos + 1);
}

bool same_path_text(const fs::path& a, const fs::path& b) {
    return a.lexically_normal().generic_string() ==
           b.lexically_normal().generic_string();
}

}  // namespace

// ── Options ─────────────────────────────────────────────────────────────────

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
        if (a == "--allow-cli-skew")   { opts.allow_cli_skew = true; continue; }
        if (a == "--allow-redundant")  { opts.allow_redundant = true; continue; }
        if (a == "--verify-builds")    { opts.verify_builds = true; continue; }
        if (a == "--to") {
            if (i + 1 >= args.size() || args[i + 1].empty()
                || args[i + 1].front() == '-') {
                std::cerr << "pulp project bump: --to requires a version argument\n";
                std::exit(2);
            }
            opts.to_version = args[++i];
            continue;
        }
        if (a == "--to=" || (a.rfind("--to=", 0) == 0 && a.size() == 5)) {
            std::cerr << "pulp project bump: --to= requires a version value (got empty)\n";
            std::exit(2);
        }
        if (auto v = strip_eq_value(a, "--to"); !v.empty()) {
            opts.to_version = v;
            continue;
        }
        if (!a.empty() && a.front() != '-') {
            opts.positional.push_back(a);
            continue;
        }
        opts.error = "pulp project bump: unknown argument '" + a + "'";
        return opts;
    }
    return opts;
}

const char* stderr_to_null() {
#if defined(_WIN32)
    return " 2>NUL";
#else
    return " 2>/dev/null";
#endif
}

const char* output_to_null() {
#if defined(_WIN32)
    return " >NUL 2>&1";
#else
    return " >/dev/null 2>&1";
#endif
}

// ── Help text ───────────────────────────────────────────────────────────────

void print_project_help() {
    std::cout <<
        "pulp project — manage a Pulp project's pinned SDK version\n\n"
        "Usage:\n"
        "  pulp project pin [<version>] [--to=X] [--all] [--dry-run]\n"
        "                   [--force-dirty] [--allow-downgrade]\n"
        "                   [--allow-cli-skew] [--allow-redundant]\n"
        "                   [--verify-builds]\n"
        "  pulp project unpin [--dry-run]\n"
        "  pulp project undo [<timestamp>]\n"
        "\n"
        "Pulp #2087: `pin` is the primary command. `bump` remains as a\n"
        "deprecated alias for one minor release. `unpin` switches the\n"
        "project to floating mode (tracks latest installed SDK).\n"
        "\n"
        "Run `pulp project pin --help`, `pulp project unpin --help`, or\n"
        "`pulp project undo --help` for command-specific details.\n";
}

void print_bump_help() {
    std::cout <<
        "pulp project pin — pin the project's Pulp SDK to a specific version\n"
        "                  (alias: `pulp project bump`)\n\n"
        "Usage:\n"
        "  pulp project pin                      Pin CWD to the CLI's own version\n"
        "  pulp project pin <version>            Pin CWD to <version> (positional)\n"
        "  pulp project pin --to=<version>       Pin CWD to <version> (named)\n"
        "  pulp project pin --all                Iterate ~/.pulp/projects.json\n"
        "  pulp project pin --all --to=<version>\n"
        "\n"
        "Flags:\n"
        "  --dry-run            Show the plan without rewriting anything\n"
        "  --force-dirty        Skip the git-clean gate (risky — changes mingle)\n"
        "  --allow-downgrade    Permit target older than current pin\n"
        "  --allow-cli-skew     Permit target newer than this pulp CLI\n"
        "  --allow-redundant    Permit pin already present on origin/main\n"
        "  --verify-builds      Build each project post-pin; roll back on failure\n"
        "\n"
        "Standalone SDK-mode projects update pulp.toml sdk_version plus a\n"
        "versioned find_package(Pulp ...) line. `project(... VERSION ...)`\n"
        "remains the plugin/app version and is not treated as the SDK pin.\n\n"
        "Every successful pin writes ~/.pulp/bump-undo-<timestamp>.json so\n"
        "`pulp project undo` can revert. Migration notes from Slice 3 (#548)\n"
        "print after the report.\n"
        "\n"
        "To go BACK to floating-SDK mode (track latest installed), run\n"
        "`pulp project unpin`. New projects created via `pulp create`\n"
        "default to floating mode (pulp #2087); pass `pulp create --pin`\n"
        "to pin at create-time instead.\n";
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

bool is_standalone_project(const fs::path& project_path) {
    return fs::exists(project_path / "pulp.toml") &&
           !fs::exists(project_path / "core");
}

bool is_pulp_source_root(const fs::path& project_path) {
    return fs::exists(project_path / "CMakeLists.txt") &&
           fs::exists(project_path / "core") &&
           fs::exists(project_path / "tools" / "cli") &&
           fs::exists(project_path / "tools" / "shipyard.toml");
}

// Check if pin-bearing files inside `project_path` have uncommitted
// changes according to `git status --porcelain`. Returns true when a
// pin file is modified, false when clean OR when git is not available
// (we refuse to block the user on a missing-tool false negative).
bool pin_files_are_dirty(const fs::path& project_path, bool standalone) {
    std::error_code ec;
    if (!fs::is_directory(project_path / ".git", ec) &&
        !fs::exists(project_path / ".git", ec)) {
        // Not a git repo — nothing to gate against.
        return false;
    }
    // Quote the project path for the shell. Use "-C <path>" so we
    // don't have to cd.
    std::string cmd = "git -C " + shell_quote(project_path) +
                      " status --porcelain -- CMakeLists.txt";
    if (standalone) cmd += " pulp.toml";
    cmd += stderr_to_null();
    auto out = trim(exec_output(cmd));
    return !out.empty();
}

pb::PinSite site_for_kind(const std::string& source, pb::PinKind kind) {
    switch (kind) {
        case pb::PinKind::PulpTomlSdkVersion:
            return pb::find_toml_string_value(source, "sdk_version", kind);
        case pb::PinKind::PulpTomlSdkPath:
            return pb::find_toml_string_value(source, "sdk_path", kind);
        case pb::PinKind::CMakeFindPackagePulpVersion:
            return pb::find_find_package_pulp_version(source);
        case pb::PinKind::FetchContentGitTag:
        case pb::PinKind::PulpAddProject:
        case pb::PinKind::ProjectVersion: {
            auto site = pb::find_pin_site(source);
            return site.kind == kind ? site : pb::PinSite{};
        }
        case pb::PinKind::Unknown:
            return {};
    }
    return {};
}

bool stage_edit(std::map<fs::path, std::string>& staged,
                const pb::UndoEdit& edit,
                const std::string& new_value) {
    auto it = staged.find(edit.path);
    if (it == staged.end()) {
        auto source = read_text(edit.path);
        if (source.empty()) return false;
        it = staged.emplace(edit.path, std::move(source)).first;
    }
    auto site = site_for_kind(it->second, edit.kind);
    if (site.kind != edit.kind) return false;
    if (site.current_pin != edit.old_value) return false;
    auto rewritten = pb::rewrite_pin(it->second, site, new_value,
                                     edit.old_value_style_has_v);
    if (!rewritten) return false;
    it->second = *rewritten;
    return true;
}

std::optional<std::string> managed_sdk_path_replacement(const fs::path& raw_path,
                                                        const std::string& current,
                                                        const std::string& target) {
    if (raw_path.empty()) return std::nullopt;
    if (same_path_text(raw_path, sdk_cache_path(current))) {
        return sdk_cache_path(target).generic_string();
    }
    if (same_path_text(raw_path, local_sdk_cache_path(current))) {
        return local_sdk_cache_path(target).generic_string();
    }
    return std::nullopt;
}

std::optional<std::string> main_pinned_version_at_origin(const fs::path& project_path,
                                                         bool standalone) {
    std::string fetch = "git -C " + shell_quote(project_path) +
                        " fetch --quiet origin main" + output_to_null();
    if (run(fetch) != 0) return std::nullopt;

    if (standalone) {
        std::string toml_cmd = "git -C " + shell_quote(project_path) +
                               " show origin/main:pulp.toml" + stderr_to_null();
        auto toml = exec_output(toml_cmd);
        if (!toml.empty()) {
            auto site = pb::find_toml_string_value(toml, "sdk_version",
                                                   pb::PinKind::PulpTomlSdkVersion);
            auto normalized = pb::normalize_pin(site.current_pin);
            if (!normalized.empty()) return normalized;
        }
    }

    std::string cmake_cmd = "git -C " + shell_quote(project_path) +
                            " show origin/main:CMakeLists.txt" + stderr_to_null();
    auto cmake = exec_output(cmake_cmd);
    if (cmake.empty()) return std::nullopt;
    auto site = standalone ? pb::find_find_package_pulp_version(cmake)
                           : pb::find_pin_site(cmake);
    auto normalized = pb::normalize_pin(site.current_pin);
    if (normalized.empty()) return std::nullopt;
    return normalized;
}

fs::path find_bumpable_project_root_from(fs::path dir, bool* is_pulp_source) {
    if (fs::is_regular_file(dir)) dir = dir.parent_path();
    if (dir.empty()) dir = fs::current_path();

    auto standalone = find_standalone_root();
    if (!standalone.empty()) return standalone;

    while (!dir.empty()) {
        if (is_pulp_source_root(dir)) {
            if (is_pulp_source) *is_pulp_source = true;
            return dir;
        }
        auto cmake = dir / "CMakeLists.txt";
        if (fs::exists(cmake)) {
            auto source = read_text(cmake);
            if (pb::find_pin_site(source).kind != pb::PinKind::Unknown) {
                return dir;
            }
        }
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
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

}  // namespace pulp_cli::project_detail
