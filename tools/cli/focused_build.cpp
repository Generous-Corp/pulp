// focused_build.cpp — C++ delegate side of focused builds. The selection logic
// lives in tools/scripts/affected_targets.py so the Rust CLI and this delegate
// share one implementation; this file runs it and reads the files it writes.
#include "focused_build.hpp"

#include "cli_common.hpp"
#include "shell_quote.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

constexpr const char* kSelectorRelative = "tools/scripts/affected_targets.py";
constexpr const char* kQueryRelative = ".cmake/api/v1/query/codemodel-v2";
constexpr const char* kWriteRelative = ".pulp/affected";

std::vector<std::string> read_lines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

}  // namespace

bool focused_build_disabled_by_env() {
    const char* value = std::getenv("PULP_BUILD_FOCUS");
    return value != nullptr && std::string(value) == "0";
}

bool build_args_name_target(const std::vector<std::string>& build_args) {
    for (const auto& arg : build_args) {
        if (arg == "--target" || arg == "-t" || arg.rfind("--target=", 0) == 0) {
            return true;
        }
    }
    return false;
}

bool focused_build_applicable(const fs::path& project_root, bool standalone,
                              const std::vector<std::string>& build_args, bool all_flag) {
    if (all_flag || standalone || focused_build_disabled_by_env()) {
        return false;
    }
    if (build_args_name_target(build_args)) {
        return false;
    }
    return fs::is_regular_file(project_root / kSelectorRelative);
}

bool ensure_codemodel_query(const fs::path& build_dir) {
    const auto query = build_dir / kQueryRelative;
    std::error_code ec;
    if (fs::exists(query, ec)) {
        return false;
    }
    fs::create_directories(query.parent_path(), ec);
    std::ofstream out(query);
    return out.good();
}

bool codemodel_reply_available(const fs::path& build_dir) {
    const auto reply = build_dir / ".cmake" / "api" / "v1" / "reply";
    std::error_code ec;
    if (!fs::is_directory(reply, ec)) {
        return false;
    }
    for (const auto& entry : fs::directory_iterator(reply, ec)) {
        if (entry.path().filename().string().rfind("index-", 0) == 0) {
            return true;
        }
    }
    return false;
}

int ensure_codemodel_reply(const fs::path& project_root, const fs::path& build_dir,
                           bool source_checkout, bool examples) {
    ensure_codemodel_query(build_dir);
    if (codemodel_reply_available(build_dir)) {
        return 0;
    }
    std::cout << "Configuring once to record the CMake codemodel that focused builds select from\n";
    std::string configure_cmd = "cmake -B " + shell_quote(build_dir.string()) + " -S "
                              + shell_quote(project_root.string())
                              + configure_default_flags(build_dir, source_checkout, examples);
    append_windows_visual_studio_generator_args(configure_cmd);
    return run_with_spinner(configure_cmd, "Configuring");
}

FocusedSelection select_affected(const fs::path& project_root, const fs::path& build_dir) {
    FocusedSelection sel;
    const auto script = project_root / kSelectorRelative;
    if (!fs::is_regular_file(script)) {
        return sel;
    }
    const auto write_dir = build_dir / kWriteRelative;
    std::error_code ec;
    fs::remove(write_dir / "banner.txt", ec);

    std::string cmd = "python3 " + shell_quote(script.string())
                    + " --build-dir " + shell_quote(build_dir.string())
                    + " --source-root " + shell_quote(project_root.string())
                    + " --write-dir " + shell_quote(write_dir.string())
                    + " --quiet";
    if (std::system(cmd.c_str()) != 0) {
        return sel;
    }

    const auto banner_lines = read_lines(write_dir / "banner.txt");
    if (banner_lines.empty()) {
        return sel;
    }
    sel.available = true;
    sel.banner = banner_lines.front();
    sel.focused = sel.banner.rfind("FOCUSED:", 0) == 0;
    if (sel.focused) {
        sel.targets = read_lines(write_dir / "targets.txt");
        sel.tests = read_lines(write_dir / "tests.txt");
        sel.tests_file = write_dir / "tests.txt";
    }
    return sel;
}

std::string focused_build_command(const std::string& build_cmd, const FocusedSelection& sel) {
    if (!sel.available || !sel.focused || sel.targets.empty()) {
        return build_cmd;
    }
    std::string cmd = build_cmd + " --target";
    for (const auto& target : sel.targets) cmd += " " + shell_quote(target);
    return cmd;
}

std::string focused_test_banner(const FocusedSelection& sel) {
    std::ostringstream out;
    out << "FOCUSED: running " << sel.tests.size()
        << " tests affected by your diff - run with --all before opening a PR";
    return out.str();
}

FocusedSelection select_for_rebuild(const fs::path& project_root, const fs::path& build_dir,
                                    bool focus, const std::string& indent) {
    FocusedSelection sel;
    if (!focus) {
        return sel;
    }
    sel = select_affected(project_root, build_dir);
    std::cout << indent
              << (sel.available ? sel.banner
                                : std::string("FULL: building all targets "
                                              "(affected-target selection unavailable)"))
              << "\n";
    return sel;
}

bool focused_nothing_to_build(const FocusedSelection& sel) {
    return sel.available && sel.focused && sel.targets.empty();
}

bool focused_test_selection(std::string& test_cmd, const std::string& test_filter,
                            const FocusedSelection& sel, const std::string& indent) {
    if (!test_filter.empty()) {
        test_cmd += " -R " + shell_quote(test_filter);
        return true;
    }
    if (!(sel.available && sel.focused)) {
        return true;
    }
    std::cout << indent << focused_test_banner(sel) << "\n";
    if (sel.tests.empty()) {
        return false;
    }
    test_cmd += " --tests-from-file " + shell_quote(sel.tests_file.string());
    return true;
}
