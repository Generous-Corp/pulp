// cli_delegate.cpp - Helper delegation for the Pulp CLI

#include "cli_common.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace {

HANDLE inheritable_stdio_handle(DWORD std_handle, DWORD null_access) {
    const auto source = GetStdHandle(std_handle);
    HANDLE inherited = INVALID_HANDLE_VALUE;
    if (source != nullptr && source != INVALID_HANDLE_VALUE) {
        if (DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &inherited,
                            0, TRUE, DUPLICATE_SAME_ACCESS)) {
            return inherited;
        }
        return INVALID_HANDLE_VALUE;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    return CreateFileA("NUL", null_access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

int run_delegate_inheriting_stdio(const fs::path& binary,
                                  const std::vector<std::string>& args) {
    std::string command_line = shell_quote(binary);
    for (const auto& arg : args) command_line += " " + shell_quote(arg);

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    const auto is_valid = [](HANDLE handle) {
        return handle != nullptr && handle != INVALID_HANDLE_VALUE;
    };
    const auto close_stdio = [&] {
        if (is_valid(startup.hStdInput)) CloseHandle(startup.hStdInput);
        if (is_valid(startup.hStdOutput)) CloseHandle(startup.hStdOutput);
        if (is_valid(startup.hStdError)) CloseHandle(startup.hStdError);
    };
    DWORD stdio_error = ERROR_SUCCESS;
    const auto prepare_stdio = [&](HANDLE& destination, DWORD source, DWORD null_access) {
        destination = inheritable_stdio_handle(source, null_access);
        if (is_valid(destination)) return true;
        stdio_error = GetLastError();
        return false;
    };
    if (!prepare_stdio(startup.hStdInput, STD_INPUT_HANDLE, GENERIC_READ) ||
        !prepare_stdio(startup.hStdOutput, STD_OUTPUT_HANDLE, GENERIC_WRITE) ||
        !prepare_stdio(startup.hStdError, STD_ERROR_HANDLE, GENERIC_WRITE)) {
        close_stdio();
        std::cerr << "Error: could not prepare delegated standard streams"
                  << " (Windows error " << stdio_error << ")\n";
        return 127;
    }

    PROCESS_INFORMATION process{};
    const auto executable = binary.string();
    if (!CreateProcessA(executable.c_str(), command_line.data(), nullptr, nullptr,
                        TRUE, 0, nullptr, nullptr, &startup, &process)) {
        const auto error = GetLastError();
        close_stdio();
        std::cerr << "Error: could not launch " << executable
                  << " (Windows error " << error << ")\n";
        return 127;
    }
    close_stdio();

    CloseHandle(process.hThread);
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_FAILED) {
        const auto error = GetLastError();
        std::cerr << "Error: could not wait for " << executable
                  << " (Windows error " << error << ")\n";
        CloseHandle(process.hProcess);
        return 1;
    }

    DWORD exit_code = 1;
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
        const auto error = GetLastError();
        std::cerr << "Error: could not read the exit status for " << executable
                  << " (Windows error " << error << ")\n";
        exit_code = 1;
    }
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}

}  // namespace
#endif

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
        // Release archives install delegate helpers beside `pulp`.
        add_candidate(cli_dir / relative_binary.filename());
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

#ifdef _WIN32
    std::vector<std::string> delegated_args;
    if (!prepend_flag.empty()) delegated_args.push_back(prepend_flag);
    delegated_args.insert(delegated_args.end(), args.begin(), args.end());
    return run_delegate_inheriting_stdio(binary, delegated_args);
#else
    std::string cmd = shell_quote(binary);
    if (!prepend_flag.empty()) cmd += " " + prepend_flag;
    for (auto& arg : args) cmd += " " + shell_quote(arg);
    return run(cmd);
#endif
}
