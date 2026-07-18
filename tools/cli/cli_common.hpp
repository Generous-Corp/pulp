// cli_common.hpp — Shared declarations for the Pulp CLI
// All command files include this header.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Split out along the implementation seam: each of these carries the
// declarations for one sibling translation unit. Included here so every
// command file that includes cli_common.hpp still sees the full CLI API.
#include "cli_doctor.hpp"   // impl: cli_doctor_helpers.cpp
#include "cli_sdk.hpp"      // impl: cli_sdk.cpp
#include "cli_aax.hpp"      // impl: cli_common.cpp / cli_doctor_helpers.cpp
#include "cli_watch.hpp"    // impl: cli_common.cpp
#include "shell_quote.hpp"

#include <pulp/runtime/system.hpp>

namespace fs = std::filesystem;

// ── SDK Constants ───────────────────────────────────────────────────────────

extern const char* PULP_SDK_VERSION;
extern const char* PULP_GITHUB_REPO;

// ── Color / Terminal ────────────────────────────────────────────────────────

extern bool g_color_enabled;
extern bool g_no_color;

bool is_tty();
void init_color();

namespace color {
std::string reset();
std::string bold();
std::string dim();
std::string green();
std::string yellow();
std::string red();
std::string cyan();
}

void print_ok(const std::string& msg);
void print_fail(const std::string& msg);
void print_warn(const std::string& msg);

// Phase-marker for `PULP_DEBUG=1` (stderr, timestamped). Silent otherwise.
// Sprinkle at user-entrypoint phases that could plausibly hang so the next
// hang report identifies the last reached phase.
void pulp_debug(const char* phase);

// ── Shell Execution ─────────────────────────────────────────────────────────

// Decode std::system()'s return into the child's exit code. On POSIX
// std::system returns a waitpid status (a normal exit of N comes back as
// N<<8), so callers that propagate it as their own exit code truncate it via
// the shell's `& 0xFF`; this normalizes it. Exposed for unit testing.
//   status == -1            → 127 (shell could not launch)
//   normal exit             → the child's exit code (WEXITSTATUS)
//   killed by signal        → 128 + signal number
// On Windows std::system already returns the child exit code, so it passes
// through unchanged. See run().
int decode_system_status(int status);

int run(const std::string& cmd);
int run_with_spinner(const std::string& cmd, const std::string& label);
std::string exec_output(const std::string& cmd);
fs::path platform_executable(fs::path p);

// ── String Utilities ────────────────────────────────────────────────────────

std::string trim(const std::string& s);
std::string strip_quotes(const std::string& s);
std::string read_file_contents(const fs::path& path);
std::string replace_all_str(const std::string& str,
                            const std::string& from,
                            const std::string& to);
bool icontains(const std::string& haystack, const std::string& needle);
std::string yaml_value(const std::string& line, const std::string& key);
std::string sanitize_process_output(std::string output);
std::string truncate_message(std::string value, std::size_t max_chars);

// ── Parsing Helpers ─────────────────────────────────────────────────────────

bool parse_size_arg(const std::string& text, const char* flag, std::size_t& out);
bool parse_double_arg(const std::string& text, const char* flag, double& out);

// ── Project Root Helpers ────────────────────────────────────────────────────

// Require being in a Pulp project. Prints error and returns nullopt if not found.
std::optional<fs::path> require_project_root();

// Like require_project_root but also checks for standalone (pulp.toml) projects.
std::optional<fs::path> require_active_project_root(bool* is_standalone = nullptr);

// ── Script/Binary Delegation ────────────────────────────────────────────────

// Run a python3 script relative to the project root, passing all args quoted.
int delegate_to_python_script(const fs::path& relative_script,
                              const std::vector<std::string>& args);

// Run a build binary relative to the project root, passing args quoted.
// Optional prepend_flag is inserted before user args (e.g., "--export-tokens").
int delegate_to_build_binary(const fs::path& relative_binary,
                             const std::vector<std::string>& args,
                             const std::string& prepend_flag = {});

// ── Path / Project Detection ────────────────────────────────────────────────

fs::path user_home_dir();
std::string find_executable_in_path(const std::string& name);
fs::path find_project_root_from(fs::path dir);
fs::path find_project_root();
fs::path find_standalone_root();
fs::path resolve_active_project_root(bool* is_standalone = nullptr);
fs::path current_executable_path();
fs::path cmake_home_directory(const fs::path& build_dir);
fs::path build_dir_from_current_binary();
bool path_is_within(const fs::path& path, const fs::path& root);
fs::path resolve_create_projects_base_dir(const fs::path& repo_root);

// ── Build Helpers ───────────────────────────────────────────────────────────

std::string default_create_formats(const fs::path& repo_root, const std::string& type);
bool checkout_supports_vst3(const fs::path& repo_root);
int ensure_repo_build_configured(const fs::path& project_root, const fs::path& build_dir);
void append_windows_visual_studio_generator_args(std::string& cmd);
#ifdef __APPLE__
bool checkout_supports_au(const fs::path& repo_root);
#endif

// ── AAX Helpers (used by validate + doctor + create) ────────────────────────
// AAX SDK / validator discovery + setup guidance moved to cli_aax.hpp
// (included above). Two generic bundle/file helpers that live in the same
// impl TUs but are not AAX-specific stay here:

fs::path write_temp_text_file(const std::string& prefix, const std::string& content);
bool bundle_contains_payload(const fs::path& bundle_path);

// ── Interactive Prompts ─────────────────────────────────────────────────────

namespace cli {

bool confirm(const std::string& question, bool default_yes = true);
int choose(const std::string& prompt, const std::vector<std::string>& options);
std::string input(const std::string& prompt, const std::string& default_value = {});

} // namespace cli

// ── File Watching / Dev Loop ────────────────────────────────────────────────
// WatchOptions + watch_loop / watch_and_rebuild moved to cli_watch.hpp
// (included above).

// ── Fuzzy Matching ──────────────────────────────────────────────────────────

int fuzzy_score(const std::string& text, const std::string& query);

// ── Command Forward Declarations (for cross-command calls) ──────────────────

int cmd_build(const std::vector<std::string>& args);
int cmd_test(const std::vector<std::string>& args);
int cmd_status(const std::vector<std::string>& args);
int cmd_clean(const std::vector<std::string>& args);
int cmd_fmt(const std::vector<std::string>& args);
int cmd_run(const std::vector<std::string>& args);
int cmd_validate(const std::vector<std::string>& args);
int cmd_ship(const std::vector<std::string>& args);
int cmd_bake(const std::vector<std::string>& args);
int cmd_doctor(const std::vector<std::string>& args);
int cmd_create(const std::vector<std::string>& args);
int cmd_docs(const std::vector<std::string>& args);
int cmd_design(const std::vector<std::string>& args);
int cmd_cache(const std::vector<std::string>& args);
int cmd_upgrade(const std::vector<std::string>& args);
int cmd_audio(const std::vector<std::string>& args);
int cmd_sdk(const std::vector<std::string>& args);
int cmd_version(const std::vector<std::string>& args);
int cmd_dev(const std::vector<std::string>& args);
int cmd_loop(const std::vector<std::string>& args);
int cmd_inspect(const std::vector<std::string>& args);
int cmd_scan(const std::vector<std::string>& args);
int cmd_host(const std::vector<std::string>& args);
int cmd_import(const std::vector<std::string>& args);
int cmd_pr(const std::vector<std::string>& args);
int cmd_projects(const std::vector<std::string>& args);
int cmd_project(const std::vector<std::string>& args);
int cmd_config(const std::vector<std::string>& args);
int cmd_coverage(const std::vector<std::string>& args);
int cmd_minos(const std::vector<std::string>& args);
int cmd_macos(const std::vector<std::string>& args);
int cmd_ci_host(const std::vector<std::string>& args);
int cmd_overflow(const std::vector<std::string>& args);
int cmd_tweaks(const std::vector<std::string>& args);
