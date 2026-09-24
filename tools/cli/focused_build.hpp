// focused_build.hpp — build only the targets the working diff affects.
//
// The Pulp source checkout has ~1,700 CMake targets; rebuilding `all` after a
// one-line edit relinks hundreds of test programs the edit cannot reach. The
// selector (`tools/scripts/affected_targets.py`) maps the working diff to the
// targets that own it plus their test programs, following add_dependencies and
// CTest fixture edges, and falls back to `all` when the diff cannot be mapped.
// This header is the C++ delegate's view of that selection: `pulp dev`,
// `pulp loop`, and `pulp-cpp build --watch` consult it before every
// `cmake --build`, and `--all`, an explicit `--target`, or PULP_BUILD_FOCUS=0
// restore the full build. Standalone SDK projects never focus: the selector
// ships with the source checkout only.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct FocusedSelection {
    bool available = false;            // the selector ran and wrote a selection
    bool focused = false;              // mode == focused (else build all)
    std::vector<std::string> targets;  // build targets (focused mode only)
    std::vector<std::string> tests;    // literal ctest names (focused mode only)
    std::string banner;                // one-line summary to print before building
    fs::path tests_file;               // literal list for `ctest --tests-from-file`
};

// True when `PULP_BUILD_FOCUS=0` opts out of focused builds.
bool focused_build_disabled_by_env();

// True when the cmake passthrough already names a target (`--target X`,
// `--target=X`, `-t X`); an explicit target always wins over the selector.
bool build_args_name_target(const std::vector<std::string>& build_args);

// Whether a focused build applies: a source checkout that ships the selector,
// no `--all`, no explicit target, no env opt-out.
bool focused_build_applicable(const fs::path& project_root, bool standalone,
                              const std::vector<std::string>& build_args, bool all_flag);

// Write the stateless CMake file-API codemodel query so the next configure
// records the target graph. Returns true when the query was newly created.
bool ensure_codemodel_query(const fs::path& build_dir);

// True once a configure has answered the codemodel query.
bool codemodel_reply_available(const fs::path& build_dir);

// Run the selector against the working diff and read back its verdict. An
// unavailable selection (script missing, failed, unreadable) means build all.
FocusedSelection select_affected(const fs::path& project_root, const fs::path& build_dir);

// `cmake --build <dir> ...` with the selection's targets appended. Unchanged
// when the selection is unavailable or `all`.
std::string focused_build_command(const std::string& build_cmd, const FocusedSelection& sel);

// Banner for a test run driven by this selection.
std::string focused_test_banner(const FocusedSelection& sel);

// Select before a (re)build when `focus` is set, printing the banner (or the
// "selection unavailable" line) prefixed by `indent`. Unfocused: empty selection.
FocusedSelection select_for_rebuild(const fs::path& project_root, const fs::path& build_dir,
                                    bool focus, const std::string& indent);

// True when the selection is focused and maps to no build target at all.
bool focused_nothing_to_build(const FocusedSelection& sel);

// Append the ctest selection to `test_cmd`: an explicit `-R` filter wins,
// else a focused selection runs its literal list. Returns false when there
// is nothing to run (focused with zero tests).
bool focused_test_selection(std::string& test_cmd, const std::string& test_filter,
                            const FocusedSelection& sel, const std::string& indent);
