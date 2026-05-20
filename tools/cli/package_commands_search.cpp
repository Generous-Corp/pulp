// SPDX-License-Identifier: MIT
//
// package_commands_search.cpp — read-only `pulp` package commands.
// Extracted from package_commands.cpp in the 2026-05 roadmap item
// P11-2 file-split.
//
// This TU holds the query / discovery sub-commands that do not mutate
// the project: cmd_target, cmd_search, cmd_list, and cmd_suggest. The
// mutating commands (add / remove / update) live in
// package_commands_add.cpp; the audit lane stays in
// package_commands.cpp. Shared helpers are reached through the private
// package_commands_internal.hpp header. Code below is moved
// byte-identically from the original package_commands.cpp.

#include "package_commands.hpp"
#include "package_commands_internal.hpp"
#include "package_registry.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <regex>

namespace pulp::cli::pkg {

// ── Commands ──

int cmd_target(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "Usage: pulp target <list|add|remove> [target]\n\n"
                  << "Manage platform targets for your project.\n"
                  << "Target format: Platform-arch (e.g., macOS-arm64, Windows-x64)\n\n"
                  << "Commands:\n"
                  << "  list              Show current project targets\n"
                  << "  add <target>      Add a platform target\n"
                  << "  remove <target>   Remove a platform target\n";
        return 0;
    }

    auto root = find_project_root();
    if (root.empty()) {
        print_fail("Not in a Pulp project (no CMakeLists.txt or pulp.toml found)");
        return 1;
    }

    auto subcmd = args[0];

    if (subcmd == "list") {
        if (args.size() > 1) {
            print_fail("Unexpected target list argument: " + args[1]);
            return 2;
        }
        auto targets = read_project_targets(root);
        bool has_toml = fs::exists(root / "pulp.toml");
        std::cout << "Project targets";
        if (!has_toml) std::cout << " " << dim("(defaults — no pulp.toml)");
        std::cout << ":\n";
        for (auto& t : targets)
            std::cout << "  " << t.to_string() << "\n";
        return 0;
    }

    if (subcmd == "add") {
        if (args.size() < 2) {
            print_fail("Usage: pulp target add <Platform-arch>");
            return 1;
        }
        if (args.size() > 2) {
            print_fail("Unexpected target add argument: " + args[2]);
            return 2;
        }
        auto parsed = PlatformTarget::parse(args[1]);
        if (!parsed) {
            print_fail("Invalid target: " + args[1]);
            std::cout << "Valid platforms: macOS, Windows, Linux, iOS, WASM\n";
            std::cout << "Valid architectures: arm64, x64, wasm32\n";
            return 1;
        }
        auto targets = read_project_targets(root);
        for (auto& t : targets) {
            if (t == *parsed) {
                print_warn("Target " + parsed->to_string() + " is already configured");
                return 0;
            }
        }
        targets.push_back(*parsed);
        if (!write_project_targets(root, targets)) {
            print_fail("Failed to write pulp.toml");
            return 1;
        }
        print_ok("Added target: " + parsed->to_string());

        // Check installed packages for compatibility
        auto lock_path = root / "packages.lock.json";
        if (fs::exists(lock_path)) {
            auto reg_path = find_registry_path(root);
            if (!reg_path.empty()) {
                auto [reg, err] = load_registry(reg_path);
                auto lock = load_lock_file(lock_path);
                for (auto& [id, lp] : lock.packages) {
                    auto it = reg.packages.find(id);
                    if (it == reg.packages.end()) continue;
                    auto unsup = unsupported_targets(it->second, {*parsed});
                    if (!unsup.empty())
                        print_warn(it->second.name + " does not support " + parsed->to_string());
                }
            }
        }
        return 0;
    }

    if (subcmd == "remove") {
        if (args.size() < 2) {
            print_fail("Usage: pulp target remove <Platform-arch>");
            return 1;
        }
        if (args.size() > 2) {
            print_fail("Unexpected target remove argument: " + args[2]);
            return 2;
        }
        auto parsed = PlatformTarget::parse(args[1]);
        if (!parsed) {
            print_fail("Invalid target: " + args[1]);
            return 1;
        }
        auto targets = read_project_targets(root);
        auto it = std::find(targets.begin(), targets.end(), *parsed);
        if (it == targets.end()) {
            print_fail("Target " + parsed->to_string() + " is not configured");
            return 1;
        }
        if (targets.size() == 1) {
            print_fail("Cannot remove the last target");
            return 1;
        }
        targets.erase(it);
        if (!write_project_targets(root, targets)) {
            print_fail("Failed to write pulp.toml");
            return 1;
        }
        print_ok("Removed target: " + parsed->to_string());
        return 0;
    }

    print_fail("Unknown target subcommand: " + subcmd);
    return 1;
}

int cmd_search(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "Usage: pulp search <query> [options]\n\n"
                  << "Search the package registry by name, description, tags, or category.\n\n"
                  << "Options:\n"
                  << "  --refresh      Force refresh the remote registry cache\n"
                  << "  --format json  Output as JSON\n";
        return 0;
    }

    bool refresh = false;
    std::string query;
    bool json_output = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--refresh") {
            refresh = true;
        } else if (args[i] == "--format") {
            if (missing_option_value(args, i)) {
                print_fail("--format requires a value");
                return 2;
            }
            if (args[i + 1] != "json") {
                print_fail("--format must be json");
                return 2;
            }
            json_output = true;
            ++i;
        } else if (looks_like_option(args[i])) {
            print_fail("Unknown search option: " + args[i]);
            return 2;
        } else {
            if (!query.empty()) query += " ";
            query += args[i];
        }
    }

    if (query.empty()) {
        print_fail("Search query required");
        return 2;
    }

    // Try local registry first, fall back to remote
    auto root = find_project_root();
    auto reg_path = root.empty() ? fs::path{} : find_registry_path(root);

    RegistryLoadResult result;
    if (!reg_path.empty()) {
        result = load_registry(reg_path);
    }

    // If local failed or empty, try remote
    if (result.registry.packages.empty()) {
        auto cache_dir = default_cache_dir();
        if (refresh) {
            result = refresh_remote_registry(default_remote_registry_url(), cache_dir);
        } else {
            result = load_remote_registry(default_remote_registry_url(), cache_dir);
        }
    }

    if (!result.error.empty() && result.registry.packages.empty()) {
        print_fail(result.error);
        return 1;
    }

    auto& reg = result.registry;

    auto results = search(reg, query);
    if (results.empty()) {
        std::cout << "No packages found matching: " << query << "\n";
        return 0;
    }

    if (json_output) {
        std::cout << "[\n";
        for (size_t i = 0; i < results.size(); ++i) {
            auto& p = *results[i];
            std::cout << "  {\"id\": \"" << p.id << "\", \"name\": \"" << p.name
                      << "\", \"version\": \"" << p.version << "\", \"license\": \""
                      << p.license << "\", \"description\": \"" << p.description << "\"}";
            if (i + 1 < results.size()) std::cout << ",";
            std::cout << "\n";
        }
        std::cout << "]\n";
    } else {
        std::cout << "Found " << results.size() << " package(s) matching \"" << query << "\":\n\n";
        for (auto* p : results) {
            auto verdict = check_license(p->license);
            std::string lic_note;
            if (verdict == LicenseVerdict::rejected) lic_note = red(" (license incompatible)");
            else if (verdict == LicenseVerdict::review_required) lic_note = yellow(" (license review required)");

            std::cout << "  " << green(p->id) << " " << dim("v" + p->version)
                      << " " << dim("[" + p->license + "]") << lic_note << "\n";
            std::cout << "    " << p->description << "\n\n";
        }
    }
    return 0;
}

int cmd_list(const std::vector<std::string>& args) {
    bool json_output = false;
    for (const auto& arg : args) {
        if (arg == "--json") {
            json_output = true;
        } else if (looks_like_option(arg)) {
            print_fail("Unknown list option: " + arg);
            return 2;
        } else {
            print_fail("Unexpected list argument: " + arg);
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
        std::cout << dim("Use 'pulp add <package>' to add a package, or 'pulp search <query>' to find packages.") << "\n";
        return 0;
    }

    auto lock = load_lock_file(lock_path);
    if (lock.packages.empty()) {
        std::cout << "No packages installed.\n";
        return 0;
    }

    // Try to enrich with registry data
    auto reg_path = find_registry_path(root);
    Registry reg;
    if (!reg_path.empty()) {
        auto [r, e] = load_registry(reg_path);
        if (e.empty()) reg = r;
    }

    if (json_output) {
        std::cout << "[\n";
        bool first = true;
        for (auto& [id, lp] : lock.packages) {
            if (!first) std::cout << ",\n";
            first = false;
            std::cout << "  {\"id\": \"" << id << "\", \"version\": \"" << lp.version << "\"}";
        }
        std::cout << "\n]\n";
    } else {
        std::cout << "Installed packages (" << lock.packages.size() << "):\n\n";
        for (auto& [id, lp] : lock.packages) {
            auto it = reg.packages.find(id);
            std::string name = it != reg.packages.end() ? it->second.name : id;
            std::string license = it != reg.packages.end() ? it->second.license : "?";
            std::string category = it != reg.packages.end() ? it->second.category : "?";

            std::cout << "  " << name << " " << dots(name) << " "
                      << dim("v" + lp.version) << " " << dim("[" + license + "]")
                      << " " << dim(category) << "\n";
        }
    }
    return 0;
}

int cmd_suggest(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cout << "Usage: pulp suggest [options]\n\n"
                  << "Options:\n"
                  << "  --description \"<text>\"   Find packages matching a description\n"
                  << "  --analyze <file>          Scan a source file for package suggestions\n"
                  << "  --alternative <package>   Find alternatives to a package\n"
                  << "  --format json             Output as JSON\n";
        return 0;
    }

    auto root = find_project_root();
    auto reg_path = root.empty() ? fs::path{} : find_registry_path(root);
    if (reg_path.empty()) {
        print_fail("Package registry not found");
        return 1;
    }

    auto [reg, err] = load_registry(reg_path);
    if (!err.empty()) { print_fail(err); return 1; }

    // Parse mode
    std::string mode, value;
    bool json_output = false;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--description") {
            if (missing_option_value(args, i)) {
                print_fail("--description requires a value");
                return 2;
            }
            mode = "description"; value = args[++i];
        } else if (args[i] == "--analyze") {
            if (missing_option_value(args, i)) {
                print_fail("--analyze requires a value");
                return 2;
            }
            mode = "analyze"; value = args[++i];
        } else if (args[i] == "--alternative") {
            if (missing_option_value(args, i)) {
                print_fail("--alternative requires a value");
                return 2;
            }
            mode = "alternative"; value = args[++i];
        } else if (args[i] == "--format") {
            if (missing_option_value(args, i)) {
                print_fail("--format requires a value");
                return 2;
            }
            if (args[i + 1] != "json") {
                print_fail("--format must be json");
                return 2;
            }
            json_output = true; ++i;
        } else if (looks_like_option(args[i])) {
            print_fail("Unknown suggest option: " + args[i]);
            return 2;
        } else {
            print_fail("Unexpected suggest argument: " + args[i]);
            return 2;
        }
    }

    if (mode == "description") {
        auto results = search(reg, value);
        if (results.empty()) {
            std::cout << (json_output ? "[]" : "No packages match that description.") << "\n";
            return 0;
        }
        if (json_output) {
            std::cout << "[\n";
            for (size_t i = 0; i < std::min(results.size(), size_t(5)); ++i) {
                auto& p = *results[i];
                if (i > 0) std::cout << ",\n";
                std::cout << "  {\"id\": \"" << p.id << "\", \"version\": \"" << p.version
                          << "\", \"license\": \"" << p.license << "\", \"description\": \""
                          << p.description << "\"}";
            }
            std::cout << "\n]\n";
        } else {
            std::cout << "Suggested packages for \"" << value << "\":\n\n";
            for (size_t i = 0; i < std::min(results.size(), size_t(5)); ++i) {
                auto& p = *results[i];
                std::cout << "  " << green(p.id) << " " << dim("v" + p.version)
                          << " " << dim("[" + p.license + "]") << "\n";
                std::cout << "    " << p.description << "\n";
                if (!p.overlaps_with_builtin.empty())
                    std::cout << "    " << yellow("Note:") << " overlaps with Pulp built-ins\n";
                std::cout << "\n";
            }
        }
        return 0;
    }

    if (mode == "analyze") {
        auto content = read_file(value);
        if (content.empty()) {
            print_fail("Cannot read file: " + value);
            return 1;
        }

        // Extract #include directives
        std::regex include_re("#include\\s*[<\"]([^>\"]+)[>\"]");
        std::sregex_iterator it(content.begin(), content.end(), include_re);
        std::sregex_iterator end;

        std::vector<std::string> includes;
        for (; it != end; ++it)
            includes.push_back((*it)[1].str());

        // Check each include against registry
        bool found_any = false;
        for (auto& [id, pkg] : reg.packages) {
            for (auto& inc : includes) {
                bool match = false;
                for (auto& tag : pkg.tags)
                    if (inc.find(tag) != std::string::npos) { match = true; break; }
                if (match) {
                    if (!found_any) {
                        std::cout << "Based on includes in " << value << ":\n\n";
                        found_any = true;
                    }
                    std::cout << "  " << green(pkg.id) << " — " << pkg.description << "\n";
                    break;
                }
            }
        }
        if (!found_any)
            std::cout << "No package suggestions based on " << value << "\n";
        return 0;
    }

    if (mode == "alternative") {
        auto it = reg.packages.find(value);
        if (it == reg.packages.end()) {
            print_fail("Package '" + value + "' not found");
            return 1;
        }

        auto& pkg = it->second;
        std::cout << "Alternatives to " << pkg.name << ":\n\n";

        // Search by provides
        for (auto& provide : pkg.provides) {
            for (auto& [id, other] : reg.packages) {
                if (id == value) continue;
                for (auto& op : other.provides) {
                    if (op == provide) {
                        std::cout << "  " << green(other.id) << " " << dim("[" + other.license + "]")
                                  << " — " << other.description << "\n";
                        break;
                    }
                }
            }
        }

        if (!pkg.alternatives.empty()) {
            std::cout << "\nAlso noted (may require commercial license):\n";
            for (auto& alt : pkg.alternatives)
                std::cout << "  " << dim(alt) << "\n";
        }
        return 0;
    }

    print_fail("Specify --description, --analyze, or --alternative");
    return 1;
}

}  // namespace pulp::cli::pkg
