// SPDX-License-Identifier: MIT
//
// package_commands.cpp — `pulp` package audit lane.
//
// Roadmap item P11-2 (2026-05) split the package CLI surface out of
// this monolith. The sub-command bodies now live in sibling TUs:
//
//   package_commands_util.cpp    — shared print/file/path helpers,
//                                  CMake-block generation, metadata edits
//   package_commands_search.cpp  — read-only commands (cmd_search,
//                                  cmd_list, cmd_suggest, cmd_target)
//   package_commands_add.cpp     — mutating commands (cmd_add,
//                                  cmd_remove, cmd_update)
//
// What remains here is the audit lane — audit_packages /
// audit_platforms / audit_licenses — invoked by `pulp audit` in
// pulp_cli.cpp. Shared helpers are reached through the private
// package_commands_internal.hpp header. The audit code below is moved
// byte-identically from the pre-split package_commands.cpp.

#include "package_commands.hpp"
#include "package_commands_internal.hpp"
#include "package_registry.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>

namespace pulp::cli::pkg {

// ── Audit Extensions ──

int audit_packages(const fs::path& project_root) {
    auto lock_path = project_root / "packages.lock.json";
    if (!fs::exists(lock_path)) {
        std::cout << "No packages installed — nothing to audit.\n";
        return 0;
    }

    auto lock = load_lock_file(lock_path);
    auto reg_path = find_registry_path(project_root);
    if (reg_path.empty()) {
        print_fail("Package registry not found");
        return 1;
    }

    auto [reg, err] = load_registry(reg_path);
    if (!err.empty()) { print_fail(err); return 1; }

    int issues = 0;
    for (auto& [id, lp] : lock.packages) {
        auto it = reg.packages.find(id);
        if (it == reg.packages.end()) {
            std::cout << "  " << id << " " << dots(id) << " " << red("NOT IN REGISTRY") << "\n";
            ++issues;
        } else {
            std::cout << "  " << id << " " << dots(id) << " " << green("OK") << "\n";
        }
    }

    std::cout << "\n" << lock.packages.size() << " packages audited, " << issues << " issues.\n";
    return issues > 0 ? 1 : 0;
}

int audit_platforms(const fs::path& project_root) {
    auto lock_path = project_root / "packages.lock.json";
    if (!fs::exists(lock_path)) {
        std::cout << "No packages installed.\n";
        return 0;
    }

    auto lock = load_lock_file(lock_path);
    auto reg_path = find_registry_path(project_root);
    if (reg_path.empty()) { print_fail("Registry not found"); return 1; }

    auto [reg, err] = load_registry(reg_path);
    if (!err.empty()) { print_fail(err); return 1; }

    auto targets = read_project_targets(project_root);
    int issues = 0;

    // Header
    std::cout << std::setw(25) << std::left << "Package";
    for (auto& t : targets) std::cout << " " << std::setw(15) << t.to_string();
    std::cout << "\n";

    for (auto& [id, lp] : lock.packages) {
        auto it = reg.packages.find(id);
        if (it == reg.packages.end()) continue;

        std::cout << std::setw(25) << std::left << id;
        auto unsup = unsupported_targets(it->second, targets);
        for (auto& t : targets) {
            bool ok = std::find(unsup.begin(), unsup.end(), t) == unsup.end();
            std::cout << " " << std::setw(15) << (ok ? green("✓") : red("✗"));
            if (!ok) ++issues;
        }
        std::cout << "\n";
    }

    if (issues > 0)
        print_warn(std::to_string(issues) + " platform gap(s) found");
    else
        print_ok("All packages support all project targets");

    return issues > 0 ? 1 : 0;
}

int audit_licenses(const fs::path& project_root) {
    auto lock_path = project_root / "packages.lock.json";
    if (!fs::exists(lock_path)) {
        std::cout << "No packages installed.\n";
        return 0;
    }

    auto lock = load_lock_file(lock_path);
    auto reg_path = find_registry_path(project_root);
    if (reg_path.empty()) { print_fail("Registry not found"); return 1; }

    auto [reg, err] = load_registry(reg_path);
    if (!err.empty()) { print_fail(err); return 1; }

    int issues = 0;
    for (auto& [id, lp] : lock.packages) {
        auto it = reg.packages.find(id);
        if (it == reg.packages.end()) continue;

        auto verdict = check_license(it->second.license);
        std::string label;
        switch (verdict) {
            case LicenseVerdict::allowed:
                label = green(it->second.license + " OK"); break;
            case LicenseVerdict::review_required:
                label = yellow(it->second.license + " REVIEW"); ++issues; break;
            case LicenseVerdict::rejected:
                label = red(it->second.license + " REJECTED"); ++issues; break;
        }
        std::cout << "  " << id << " " << dots(id) << " " << label << "\n";
    }

    std::cout << "\n" << lock.packages.size() << " packages checked, " << issues << " issues.\n";
    return issues > 0 ? 1 : 0;
}

}  // namespace pulp::cli::pkg
