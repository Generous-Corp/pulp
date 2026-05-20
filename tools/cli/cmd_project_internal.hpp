// cmd_project_internal.hpp — private shared surface for the `pulp project`
// sub-command translation units.
//
// Roadmap item P11-1: `cmd_project.cpp` was a single ~1,047-line file
// holding the bump/pin, undo, and unpin handlers plus a pile of shared
// pin-rewrite helpers. It is split into focused sibling TUs:
//
//   cmd_project.cpp          — dispatcher (`cmd_project`) + `do_unpin`
//   cmd_project_common.cpp   — shared pin-file / project-root helpers
//   cmd_project_bump.cpp     — `do_bump` (pin / bump)
//   cmd_project_undo.cpp     — `do_undo`
//
// This header is the PRIVATE internal surface — it lives next to the
// .cpp files under tools/cli/, NOT in any public include tree. It
// declares only the small one-directional entry-point set the
// dispatcher and the per-command TUs need to call across the split.

#pragma once

#include "cli_common.hpp"
#include "project_bump.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pulp_cli::project_detail {

namespace pb = pulp::cli::project_bump;

// ── Options ─────────────────────────────────────────────────────────────────

struct BumpOptions {
    std::string to_version;          // may be empty → default to CLI version
    bool all = false;
    bool dry_run = false;
    bool force_dirty = false;
    bool allow_downgrade = false;
    bool allow_cli_skew = false;
    bool allow_redundant = false;
    bool verify_builds = false;      // opt-in, also honors update.verify_builds
    std::string error;
    std::vector<std::string> positional;
};

BumpOptions parse_bump_options(const std::vector<std::string>& args,
                               bool& out_help);

// ── Platform redirect snippets ──────────────────────────────────────────────

const char* stderr_to_null();
const char* output_to_null();

// ── Help text ───────────────────────────────────────────────────────────────

void print_project_help();
void print_bump_help();
void print_undo_help();

// ── Pin-file / project-root helpers ─────────────────────────────────────────

std::string read_text(const fs::path& p);
bool write_text_atomic(const fs::path& path, const std::string& body);
bool is_standalone_project(const fs::path& project_path);
bool is_pulp_source_root(const fs::path& project_path);
bool pin_files_are_dirty(const fs::path& project_path, bool standalone);
pb::PinSite site_for_kind(const std::string& source, pb::PinKind kind);
bool stage_edit(std::map<fs::path, std::string>& staged,
                const pb::UndoEdit& edit,
                const std::string& new_value);
std::optional<std::string> managed_sdk_path_replacement(const fs::path& raw_path,
                                                        const std::string& current,
                                                        const std::string& target);
std::optional<std::string> main_pinned_version_at_origin(const fs::path& project_path,
                                                         bool standalone);
fs::path find_bumpable_project_root_from(fs::path dir, bool* is_pulp_source);
std::string fmt_pin(const std::string& raw, bool has_v);

// ── Sub-command entry points ────────────────────────────────────────────────

int do_bump(const std::vector<std::string>& args);
int do_undo(const std::vector<std::string>& args);

}  // namespace pulp_cli::project_detail
