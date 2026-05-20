// SPDX-License-Identifier: MIT
//
// package_commands_add.cpp — mutating `pulp` package commands.
// Extracted from package_commands.cpp in the 2026-05 roadmap item
// P11-2 file-split.
//
// This TU holds the sub-commands that mutate the project's lock file,
// generated CMake, and dependency metadata: cmd_add, cmd_remove, and
// cmd_update. The read-only query commands (search / list / suggest /
// target) live in package_commands_search.cpp; the audit lane stays
// in package_commands.cpp. Shared helpers — including CMake-block
// generation and the DEPENDENCIES.md / NOTICE.md edits — are reached
// through the private package_commands_internal.hpp header. Code below
// is moved byte-identically from the original package_commands.cpp.

#include "package_commands.hpp"
#include "package_commands_internal.hpp"
#include "package_registry.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <sstream>

namespace pulp::cli::pkg {

int cmd_add(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "Usage: pulp add <package> [options]\n\n"
                  << "Options:\n"
                  << "  --accept-license <SPDX>         Accept a copyleft license (e.g., GPL-3.0, AGPL-3.0)\n"
                  << "  --license-override commercial   Accept with a commercial license\n"
                  << "  --platform-guard                Add with platform guard (skip prompt)\n"
                  << "  --no-cmake                      Skip CMake wiring\n";
        return 0;
    }

    // Parse args
    std::string package_id;
    bool license_override = false;
    std::string accepted_license;
    bool platform_guard = false;
    bool no_cmake = false;

    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--accept-license") {
            if (missing_option_value(args, i)) {
                print_fail("--accept-license requires a value");
                return 2;
            }
            accepted_license = args[++i]; license_override = true;
        } else if (args[i] == "--license-override") {
            if (missing_option_value(args, i)) {
                print_fail("--license-override requires a value");
                return 2;
            }
            if (args[i + 1] != "commercial") {
                print_fail("--license-override must be commercial");
                return 2;
            }
            license_override = true; ++i;
        } else if (args[i] == "--platform-guard") {
            platform_guard = true;
        } else if (args[i] == "--no-cmake") {
            no_cmake = true;
        } else if (package_id.empty() && !args[i].starts_with("-")) {
            package_id = args[i];
        } else if (looks_like_option(args[i])) {
            print_fail("Unknown add option: " + args[i]);
            return 2;
        } else {
            print_fail("Unexpected add argument: " + args[i]);
            return 2;
        }
    }

    if (package_id.empty()) {
        print_fail("No package specified");
        return 1;
    }

    auto root = find_project_root();
    if (root.empty()) {
        print_fail("Not in a Pulp project");
        return 1;
    }

    // Load registry
    auto reg_path = find_registry_path(root);
    if (reg_path.empty()) {
        print_fail("Package registry not found at tools/packages/registry.json");
        return 1;
    }

    auto [reg, reg_err] = load_registry(reg_path);
    if (!reg_err.empty()) {
        print_fail(reg_err);
        return 1;
    }

    // Find package
    auto it = reg.packages.find(package_id);
    if (it == reg.packages.end()) {
        print_fail("Package '" + package_id + "' not found in registry");
        // Suggest similar
        auto results = search(reg, package_id);
        if (!results.empty()) {
            std::cout << "\nDid you mean:\n";
            for (size_t i = 0; i < std::min(results.size(), size_t(3)); ++i)
                std::cout << "  " << results[i]->id << " — " << results[i]->description << "\n";
        }
        return 1;
    }

    auto& pkg = it->second;

    // License check
    auto verdict = check_license(pkg.license);
    auto tier = license_tier(pkg.license);

    if (verdict == LicenseVerdict::rejected && !license_override) {
        if (tier == "restricted") {
            // Copyleft — can be accepted with --accept-license
            print_fail(pkg.name + " is " + pkg.license + " licensed (copyleft).");
            std::cout << "\n  " << license_explanation(pkg.license) << "\n\n";
            std::cout << "  This is appropriate if:\n"
                      << "  " << dim("• Your plugin is open-source under a GPL-compatible license") << "\n"
                      << "  " << dim("• You have a commercial license from the library author") << "\n"
                      << "  " << dim("• You're doing research/prototyping and won't distribute") << "\n\n";
            std::cout << "  To proceed:\n"
                      << "    pulp add " << package_id << " --accept-license " << pkg.license << "\n";
            // Suggest MIT alternatives
            auto alts = search(reg, package_id);
            bool showed_alt = false;
            for (auto* alt : alts) {
                if (alt->id != package_id && check_license(alt->license) == LicenseVerdict::allowed) {
                    if (!showed_alt) std::cout << "\n  MIT-compatible alternative:\n";
                    std::cout << "    pulp add " << alt->id << "  " << dim("(" + alt->license + ")") << "\n";
                    showed_alt = true;
                    break;
                }
            }
            return 1;
        } else {
            // Truly rejected (SSPL, proprietary)
            print_fail(pkg.name + " is " + pkg.license + " licensed — cannot be used.");
            return 1;
        }
    }
    if (verdict == LicenseVerdict::rejected && license_override) {
        // Validate --accept-license matches the actual package license
        if (!accepted_license.empty() && accepted_license != pkg.license) {
            print_fail("--accept-license " + accepted_license + " does not match package license " + pkg.license);
            return 1;
        }
        print_warn("Installing " + pkg.license + " package — your distributed binary must comply with " + pkg.license);
    }
    if (verdict == LicenseVerdict::review_required) {
        print_warn(pkg.name + " license (" + pkg.license + ") requires manual review");
    }

    // Check if already installed
    auto lock_path = root / "packages.lock.json";
    auto lock = load_lock_file(lock_path);
    auto lock_it = lock.packages.find(package_id);
    if (lock_it != lock.packages.end()) {
        if (lock_it->second.version == pkg.version) {
            std::cout << pkg.name << " v" << pkg.version << " is already installed.\n";
            return 0;
        }
        print_warn(pkg.name + " is installed at v" + lock_it->second.version
                   + ", registry has v" + pkg.version + ". Use 'pulp update' to upgrade.");
        return 0;
    }

    // Platform check
    auto targets = read_project_targets(root);
    auto unsup = unsupported_targets(pkg, targets);
    if (!unsup.empty()) {
        print_warn(pkg.name + " does not support all your project targets:");
        for (auto& t : unsup)
            std::cout << "  " << red("✗") << " " << t.to_string() << "\n";

        if (!platform_guard) {
            std::cout << "\nOptions:\n"
                      << "  1. Add with platform guard (compile only on supported platforms)\n"
                      << "  2. Cancel\n"
                      << "Use --platform-guard to skip this prompt.\n";
            return 1;
        }
    }

    // Overlap check
    if (!pkg.overlaps_with_builtin.empty()) {
        print_warn(pkg.name + " overlaps with Pulp built-ins:");
        for (auto& [header, desc] : pkg.overlaps_with_builtin)
            std::cout << "  " << dim(header) << " — " << desc << "\n";
        std::cout << "  " << green("Unique value:") << " " << pkg.unique_value << "\n";
    }

    if (!no_cmake) {
        // Generate/update cmake/pulp-packages.cmake
        lock.packages[package_id] = {pkg.version, pkg.fetch.git_repository, "", pkg.fetch.git_tag};

        // If platform_guard is set and package doesn't support all targets,
        // write a guarded cmake block covering all supported desktop platforms
        bool needs_guard = platform_guard && !unsup.empty();
        std::string guard_cmake;
        if (needs_guard) {
            // Build a compound CMake guard for all supported desktop platforms
            std::vector<std::string> conditions;
            for (auto& [name, ps] : pkg.platforms) {
                if (name == "macOS") conditions.push_back("APPLE");
                else if (name == "Windows") conditions.push_back("WIN32");
                else if (name == "Linux") conditions.push_back("UNIX AND NOT APPLE");
            }
            if (!conditions.empty()) {
                std::string guard_condition;
                for (size_t i = 0; i < conditions.size(); ++i) {
                    if (i > 0) guard_condition += " OR ";
                    guard_condition += conditions[i];
                }
                // Generate block with custom compound guard
                std::ostringstream os;
                os << "# ── " << pkg.id << " (" << pkg.version << ") [platform guard] ──\n";
                os << "if(" << guard_condition << ")\n";
                os << "  FetchContent_Declare(" << pkg.id << "\n";
                os << "    GIT_REPOSITORY " << pkg.fetch.git_repository << "\n";
                os << "    GIT_TAG        " << pkg.fetch.git_tag << "\n";
                os << "    GIT_SHALLOW    TRUE\n";
                os << "  )\n";
                os << "  FetchContent_MakeAvailable(" << pkg.id << ")\n";
                // Header-only packages need INTERFACE target + include dir
                if (pkg.cmake.header_only && !pkg.cmake.targets.empty()) {
                    os << "  add_library(" << pkg.cmake.targets[0] << " INTERFACE)\n";
                    os << "  target_include_directories(" << pkg.cmake.targets[0]
                       << " INTERFACE ${" << pkg.id << "_SOURCE_DIR}";
                    if (!pkg.cmake.include_dir.empty() && pkg.cmake.include_dir != ".")
                        os << "/" << pkg.cmake.include_dir;
                    os << ")\n";
                }
                auto upper_id = pkg.id;
                std::transform(upper_id.begin(), upper_id.end(), upper_id.begin(),
                    [](unsigned char c) { return c == '-' ? '_' : std::toupper(c); });
                os << "  target_compile_definitions(${PROJECT_NAME} PRIVATE PULP_HAS_"
                   << upper_id << "=1)\n";
                os << "endif()\n";
                os << "# ── end " << pkg.id << " ──\n";
                guard_cmake = os.str();
            }
        }

        auto cmake_content = generate_packages_cmake(lock, reg, root);
        // If we have a guarded block, replace the unguarded one
        if (needs_guard && !guard_cmake.empty()) {
            auto unguarded = generate_cmake_block(pkg);
            auto pos = cmake_content.find(unguarded);
            if (pos != std::string::npos)
                cmake_content.replace(pos, unguarded.size(), guard_cmake);
        }
        auto cmake_path = root / "cmake" / "pulp-packages.cmake";
        if (!write_file(cmake_path, cmake_content)) {
            print_fail("Failed to write " + cmake_path.string());
            return 1;
        }

        // Ensure CMakeLists.txt includes the packages file
        ensure_cmake_include(root);
    }

    // Update lock file
    lock.packages[package_id] = {pkg.version, pkg.fetch.git_repository, "", pkg.fetch.git_tag};
    if (!save_lock_file(lock_path, lock)) {
        print_fail("Failed to write packages.lock.json");
        return 1;
    }

    // Update metadata files
    update_dependencies_md(root, pkg, true);
    update_notice_md(root, pkg, true);

    // Print success
    print_ok("Added " + pkg.name + " v" + pkg.version);

    if (!no_cmake && !pkg.cmake.targets.empty()) {
        std::cout << "\n  Add to your CMakeLists.txt target:\n";
        std::cout << "    target_link_libraries(YourTarget PRIVATE " << pkg.cmake.targets[0] << ")\n";
    }

    return 0;
}

int cmd_remove(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "Usage: pulp remove <package>\n";
        return 0;
    }
    if (args[0].starts_with("-")) {
        print_fail("Unknown remove option: " + args[0]);
        return 2;
    }
    if (args.size() > 1) {
        print_fail("Unexpected remove argument: " + args[1]);
        return 2;
    }

    auto root = find_project_root();
    if (root.empty()) {
        print_fail("Not in a Pulp project");
        return 1;
    }

    auto package_id = args[0];
    auto lock_path = root / "packages.lock.json";
    auto lock = load_lock_file(lock_path);

    if (lock.packages.find(package_id) == lock.packages.end()) {
        print_fail("Package '" + package_id + "' is not installed");
        return 1;
    }

    // Get package info for metadata cleanup
    auto reg_path = find_registry_path(root);
    PackageDescriptor pkg;
    pkg.id = package_id;
    if (!reg_path.empty()) {
        auto [reg, err] = load_registry(reg_path);
        auto it = reg.packages.find(package_id);
        if (it != reg.packages.end()) pkg = it->second;
    }

    // Remove from lock file
    lock.packages.erase(package_id);
    save_lock_file(lock_path, lock);

    // Regenerate cmake/pulp-packages.cmake
    if (!reg_path.empty()) {
        auto [reg, err] = load_registry(reg_path);
        auto cmake_content = generate_packages_cmake(lock, reg, root);
        auto cmake_path = root / "cmake" / "pulp-packages.cmake";
        if (lock.packages.empty()) {
            fs::remove(cmake_path);
        } else {
            write_file(cmake_path, cmake_content);
        }
    }

    // Update metadata
    if (!pkg.name.empty()) {
        update_dependencies_md(root, pkg, false);
        update_notice_md(root, pkg, false);
    }

    print_ok("Removed " + (pkg.name.empty() ? package_id : pkg.name));
    std::cout << dim("  Remember to remove target_link_libraries and #include directives from your code.") << "\n";
    return 0;
}

int cmd_update(const std::vector<std::string>& args) {
    bool apply = false;
    for (const auto& arg : args) {
        if (arg == "--apply") {
            apply = true;
        } else if (looks_like_option(arg)) {
            print_fail("Unknown update option: " + arg);
            return 2;
        } else {
            print_fail("Unexpected update argument: " + arg);
            return 2;
        }
    }

    auto root = find_project_root();
    if (root.empty()) {
        print_fail("Not in a Pulp project");
        return 1;
    }

    auto lock_path = root / "packages.lock.json";
    if (!fs::exists(lock_path)) {
        std::cout << "No packages installed.\n";
        return 0;
    }

    auto reg_path = find_registry_path(root);
    if (reg_path.empty()) {
        print_fail("Package registry not found");
        return 1;
    }

    auto [reg, err] = load_registry(reg_path);
    if (!err.empty()) { print_fail(err); return 1; }

    auto lock = load_lock_file(lock_path);
    bool has_updates = false;

    for (auto& [id, lp] : lock.packages) {
        auto it = reg.packages.find(id);
        if (it == reg.packages.end()) continue;

        if (lp.version != it->second.version) {
            has_updates = true;
            std::cout << "  " << it->second.name << ": "
                      << dim(lp.version) << " → " << green(it->second.version) << "\n";
            if (apply) {
                lp.version = it->second.version;
                lp.commit = it->second.fetch.git_tag;
            }
        }
    }

    if (!has_updates) {
        print_ok("All packages are up to date");
        return 0;
    }

    if (apply) {
        save_lock_file(lock_path, lock);
        auto cmake_content = generate_packages_cmake(lock, reg, root);
        write_file(root / "cmake" / "pulp-packages.cmake", cmake_content);
        print_ok("Updated packages and regenerated cmake/pulp-packages.cmake");
    } else {
        std::cout << "\nRun 'pulp update --apply' to apply these updates.\n";
    }

    return 0;
}

}  // namespace pulp::cli::pkg
