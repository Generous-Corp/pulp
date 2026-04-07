// cmd_run.cpp — pulp run command

#include "cli_common.hpp"

#include <iostream>

int cmd_run(const std::vector<std::string>& args) {
    bool standalone_mode = false;
    auto root = resolve_active_project_root(&standalone_mode);
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    std::string target_name;
    std::vector<std::string> pass_through;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            std::cout << "pulp run — launch a standalone Pulp application\n\n";
            std::cout << "Usage: pulp run [target] [-- args...]\n\n";
            std::cout << "If no target is specified, finds the first standalone binary in the active project build.\n";
            std::cout << "Arguments after -- are passed to the launched application.\n";
            return 0;
        }
        if (args[i] == "--") {
            for (size_t j = i + 1; j < args.size(); ++j)
                pass_through.push_back(args[j]);
            break;
        }
        if (target_name.empty() && args[i][0] != '-') {
            target_name = args[i];
        }
    }

    auto build_dir = root / "build";
    if (!fs::exists(build_dir / "CMakeCache.txt")) {
        std::cerr << "Error: project not built yet. Run `pulp build` first.\n";
        return 1;
    }

    // Find standalone binary
    fs::path binary;
    auto app_search_root = standalone_mode ? (build_dir / "bin") : (build_dir / "examples");

#ifdef __APPLE__
    auto find_app_bundle = [&](const fs::path& search_dir) -> fs::path {
        if (!fs::exists(search_dir)) return {};
        for (auto& entry : fs::directory_iterator(search_dir)) {
            if (!entry.is_directory()) continue;
            auto name = entry.path().filename().string();
            if (name.size() > 4 && name.substr(name.size() - 4) == ".app") {
                auto macos_dir = entry.path() / "Contents" / "MacOS";
                if (fs::exists(macos_dir)) {
                    for (auto& exec_entry : fs::directory_iterator(macos_dir)) {
                        if (!exec_entry.is_regular_file()) continue;
                        auto st = fs::status(exec_entry.path());
                        if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                            return exec_entry.path();
                        }
                    }
                }
            }
        }
        return {};
    };
#endif

    if (!target_name.empty()) {
        if (standalone_mode) {
            if (fs::exists(app_search_root)) {
                for (auto& file : fs::directory_iterator(app_search_root)) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname == target_name || file.path().stem().string() == target_name) {
                        auto st = fs::status(file.path());
                        if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                            binary = file.path();
                            break;
                        }
                    }
                }
            }
        } else if (fs::exists(app_search_root)) {
            for (auto& dir_entry : fs::directory_iterator(app_search_root)) {
                if (!dir_entry.is_directory()) continue;
                for (auto& file : fs::directory_iterator(dir_entry.path())) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname == target_name || file.path().stem().string() == target_name) {
                        auto st = fs::status(file.path());
                        if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                            binary = file.path();
                            break;
                        }
                    }
                }
                if (!binary.empty()) break;
            }
        }
        if (binary.empty()) {
            std::cerr << "Error: could not find standalone binary '" << target_name
                      << "' in " << app_search_root.string() << "\n";
            std::cerr << "  Run `pulp build` to build, then try again.\n";
            return 1;
        }
    } else {
        if (standalone_mode) {
            if (fs::exists(app_search_root)) {
                for (auto& file : fs::directory_iterator(app_search_root)) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname.find("cmake") != std::string::npos) continue;
                    if (fname.find(".") != std::string::npos) continue;
                    auto st = fs::status(file.path());
                    if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                        binary = file.path();
                        break;
                    }
                }
            }
        } else if (fs::exists(app_search_root)) {
            for (auto& dir_entry : fs::directory_iterator(app_search_root)) {
                if (!dir_entry.is_directory()) continue;
                for (auto& file : fs::directory_iterator(dir_entry.path())) {
                    if (!file.is_regular_file()) continue;
                    auto fname = file.path().filename().string();
                    if (fname.find("-test") != std::string::npos) continue;
                    if (fname.find("cmake") != std::string::npos) continue;
                    if (fname.find(".") != std::string::npos) continue;
                    auto st = fs::status(file.path());
                    if ((st.permissions() & fs::perms::owner_exec) != fs::perms::none) {
                        binary = file.path();
                        break;
                    }
                }
                if (!binary.empty()) break;
            }
        }
#ifdef __APPLE__
        if (binary.empty()) {
            binary = find_app_bundle(build_dir);
        }
        if (binary.empty()) {
            binary = find_app_bundle(app_search_root);
        }
#endif
        if (binary.empty()) {
            std::cerr << "Error: no standalone binary found in " << app_search_root.string() << "\n";
            std::cerr << "  Create one with: pulp create MyApp --type app\n";
            std::cerr << "  Or build an existing project: pulp build\n";
            return 1;
        }
    }

    std::cout << "Launching " << binary.filename().string() << "...\n";

    std::string cmd = binary.string();
    for (auto& arg : pass_through) {
        cmd += " \"" + arg + "\"";
    }

    return run(cmd);
}
