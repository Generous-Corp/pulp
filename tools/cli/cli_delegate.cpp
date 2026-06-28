// cli_delegate.cpp - Helper delegation for the Pulp CLI

#include "cli_common.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>

int delegate_to_python_script(const fs::path& relative_script,
                              const std::vector<std::string>& args) {
    auto root = require_project_root();
    if (!root) return 1;
    auto script = *root / relative_script;
    if (!fs::exists(script)) {
        std::cerr << "Error: script not found at " << script.string() << "\n";
        return 1;
    }
    std::string cmd = "python3 " + shell_quote(script);
    for (auto& arg : args) cmd += " " + shell_quote(arg);
    return run(cmd);
}

int delegate_to_build_binary(const fs::path& relative_binary,
                             const std::vector<std::string>& args,
                             const std::string& prepend_flag) {
    // pulp #-friction-1+#-friction-2 - delegate lookup must NOT require
    // cwd to be inside a Pulp project. Sibling binaries (pulp-import-design,
    // pulp-design-debug) live next to the CLI binary itself, so we can
    // resolve them from argv[0] alone. Only fall back to the project root
    // when the self-path lookup misses (e.g. when an installed `pulp` is
    // dispatching to a build dir).
    std::vector<fs::path> candidates;
    auto add_candidate = [&](fs::path path) {
        path = platform_executable(std::move(path));
        for (const auto& existing : candidates) {
            if (existing == path) return;
        }
        candidates.push_back(std::move(path));
    };
    auto is_config_dir = [](const fs::path& name) {
        const auto text = name.string();
        return text == "Release" || text == "Debug" ||
               text == "RelWithDebInfo" || text == "MinSizeRel";
    };
    auto add_build_candidates = [&](const fs::path& build_dir,
                                    const std::string& preferred_config = {}) {
        if (build_dir.empty()) return;

        add_candidate(build_dir / relative_binary);

        const auto parent = relative_binary.parent_path();
        const auto leaf = relative_binary.filename();
        std::vector<std::string> configs;
        auto add_config = [&](std::string config) {
            if (config.empty()) return;
            if (std::find(configs.begin(), configs.end(), config) != configs.end()) {
                return;
            }
            configs.push_back(std::move(config));
        };
        add_config(preferred_config);
        add_config("Release");
        add_config("RelWithDebInfo");
        add_config("Debug");
        add_config("MinSizeRel");
        for (const auto& config : configs) {
            add_candidate(build_dir / parent / config / leaf);
        }
    };

    // Dev/CI builds can use matrix-scoped build directories such as
    // build-linux or build-macos. Resolve sibling helper binaries from the
    // running CLI's build tree before falling back to the legacy build/ path.
    auto self = current_executable_path();
    if (!self.empty()) {
        auto cli_dir = self.parent_path();
        std::string preferred_config;
        if (is_config_dir(cli_dir.filename())) {
            preferred_config = cli_dir.filename().string();
            cli_dir = cli_dir.parent_path();
        }
        auto tools_dir = cli_dir.parent_path();
        auto build_dir = tools_dir.parent_path();
        if (cli_dir.filename() == "cli" && tools_dir.filename() == "tools" &&
            !build_dir.empty()) {
            add_build_candidates(build_dir, preferred_config);
        }
    }

    // Project-root-relative paths (and $PULP_BUILD_DIR overrides) are only
    // meaningful when the user is operating inside a Pulp checkout.
    // find_project_root() returns empty when there isn't one; we tolerate
    // that and rely on the self-path candidate above.
    fs::path root = find_project_root();  // empty if outside a project

    if (const char* env = std::getenv("PULP_BUILD_DIR"); env && *env) {
        fs::path build_dir(env);
        if (build_dir.is_relative() && !root.empty()) {
            build_dir = root / build_dir;
        }
        if (!build_dir.is_relative()) {
            add_build_candidates(build_dir);
        }
    }

    if (!root.empty()) {
        add_build_candidates(root / "build");
    }

    fs::path binary;
    for (const auto& candidate : candidates) {
        if (fs::exists(candidate)) {
            binary = candidate;
            break;
        }
    }

    if (binary.empty()) {
        const auto leaf = fs::path(relative_binary).filename().string();
        std::cerr << "Error: " << leaf << " helper not found.\n";
        std::cerr << "  Looked in:\n";
        for (const auto& c : candidates) {
            std::cerr << "    " << c.string() << "\n";
        }
        std::cerr << "  Fix: from a Pulp checkout, run\n"
                  << "    cmake --build build --target " << leaf << "\n";
        if (root.empty()) {
            std::cerr << "  (cwd is not inside a Pulp project; set PULP_BUILD_DIR or\n"
                      << "  run from a checkout to use a project-relative build dir)\n";
        }
        return 1;
    }

    std::string cmd = shell_quote(binary);
    if (!prepend_flag.empty()) cmd += " " + prepend_flag;
    for (auto& arg : args) cmd += " " + shell_quote(arg);
    return run(cmd);
}
