// cli_common.hpp — Shared declarations for the Pulp CLI
// All command files include this header.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

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

// ── Shell Execution ─────────────────────────────────────────────────────────

int run(const std::string& cmd);
int run_with_spinner(const std::string& cmd, const std::string& label);
std::string exec_output(const std::string& cmd);
std::string shell_quote(const std::string& s);
std::string shell_quote(const fs::path& p);
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

// ── SDK / Config ────────────────────────────────────────────────────────────

fs::path pulp_home();
fs::path sdk_cache_path(const std::string& version);
fs::path local_sdk_cache_path(const std::string& version);
std::string detect_platform();
fs::path ensure_sdk(const std::string& version);
fs::path ensure_checkout_sdk(const fs::path& repo_root, const std::string& version);
int ensure_checkout_dependencies(const fs::path& repo_root);
std::string read_pulp_toml_value(const fs::path& project_root, const std::string& key);
std::string read_sdk_version(const fs::path& project_root);
fs::path read_sdk_path_hint(const fs::path& project_root);
fs::path read_sdk_checkout_hint(const fs::path& project_root);
std::string read_user_config_value(const std::string& section, const std::string& key);

// Write/update `key = "value"` under `[section]` in ~/.pulp/config.toml.
// Creates the file if missing. Preserves all other content verbatim.
// Release-discovery Slice 2 (#547) surface — used by `pulp config set`
// and by the banner-suppression bookkeeping inside cmd_upgrade.
bool write_user_config_value(const std::string& section,
                             const std::string& key,
                             const std::string& value);
std::string read_project_cmake_version(const fs::path& project_root);

// ── Build Helpers ───────────────────────────────────────────────────────────

std::string default_create_formats(const fs::path& repo_root, const std::string& type);
bool checkout_supports_vst3(const fs::path& repo_root);
int ensure_repo_build_configured(const fs::path& project_root, const fs::path& build_dir);
void append_windows_visual_studio_generator_args(std::string& cmd);
#ifdef __APPLE__
bool checkout_supports_au(const fs::path& repo_root);
#endif

// ── AAX Helpers (used by validate + doctor + create) ────────────────────────

bool aax_supported_on_host();
std::string aax_download_url();
std::string aax_sdk_download_label();
std::string aax_validator_download_label();
bool looks_like_aax_sdk_root(const fs::path& path);
fs::path find_aax_sdk_root();
fs::path find_aax_validator_root();
void print_aax_setup_guidance(bool need_sdk, bool need_validator);
fs::path write_temp_text_file(const std::string& prefix, const std::string& content);
bool bundle_contains_payload(const fs::path& bundle_path);
std::string run_aax_validator_command(const fs::path& validator_root,
                                     const fs::path& plugin_path,
                                     bool run_all);
bool aax_validator_passed(const std::string& output);

// ── Doctor (shared by doctor + create) ──────────────────────────────────────

struct DoctorCheck {
    std::string name;
    bool passed;
    std::string detail;
    std::string fix;
    // Optional checks report remediation advice but don't contribute
    // to the overall doctor exit code when they fail. Used for e.g.
    // the Google Android CLI accelerator (#355) which is a speedup,
    // not a requirement. See #438 P1 for #389.
    bool optional = false;
};

std::vector<DoctorCheck> run_doctor_checks(const fs::path& active_root, bool standalone_mode);

// `pulp doctor android` — Android NDK / SDK / emulator checks plus
// optional Google "Android CLI" detection (#355). Passes the host
// platform implicitly via #ifdef in the implementation.
std::vector<DoctorCheck> run_doctor_android_checks();

// `pulp doctor ios` — Xcode + iOS Simulator checks. macOS-only;
// returns a single explanatory entry on other hosts so users still
// see a useful message.
std::vector<DoctorCheck> run_doctor_ios_checks();

// ── Interactive Prompts ─────────────────────────────────────────────────────

namespace cli {

bool confirm(const std::string& question, bool default_yes = true);
int choose(const std::string& prompt, const std::vector<std::string>& options);
std::string input(const std::string& prompt, const std::string& default_value = {});

} // namespace cli

// ── File Watching / Dev Loop ────────────────────────────────────────────────

struct WatchOptions {
    fs::path root;
    fs::path build_dir;
    std::vector<std::string> build_args;
    bool run_tests = false;          // run ctest after successful build
    std::string test_filter;          // -R filter for ctest (empty = all)
    bool run_validate = false;        // run quick dlopen validation after build
    std::string launch_target;        // binary to launch/relaunch (empty = none)
    std::vector<std::string> launch_args;  // args for launched binary
};

// Watch source files and rebuild on changes. Optionally runs tests,
// validation, and manages a launched subprocess.
int watch_loop(const WatchOptions& opts);

// Legacy API — calls watch_loop with build-only options
int watch_and_rebuild(const fs::path& root, const fs::path& build_dir,
                      const std::vector<std::string>& build_args);

// ── Fuzzy Matching ──────────────────────────────────────────────────────────

int fuzzy_score(const std::string& text, const std::string& query);

// ── Command Forward Declarations (for cross-command calls) ──────────────────

int cmd_build(const std::vector<std::string>& args);
int cmd_test(const std::vector<std::string>& args);
int cmd_status(const std::vector<std::string>& args);
int cmd_clean(const std::vector<std::string>& args);
int cmd_run(const std::vector<std::string>& args);
int cmd_validate(const std::vector<std::string>& args);
int cmd_ship(const std::vector<std::string>& args);
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
int cmd_scan(const std::vector<std::string>& args);
int cmd_host(const std::vector<std::string>& args);
int cmd_pr(const std::vector<std::string>& args);
int cmd_config(const std::vector<std::string>& args);
