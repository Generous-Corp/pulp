// cmd_version.cpp — pulp version command
// Show, bump, and verify version consistency across all surfaces.

#include "cli_common.hpp"

#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>

// ── Helpers ─────────────────────────────────────────────────────────────────

static bool write_cmake_project_version(const fs::path& cmake_path,
                                         const std::string& old_ver,
                                         const std::string& new_ver,
                                         bool plugin_only = false) {
    auto content = read_file_contents(cmake_path);
    if (plugin_only) {
        // Only replace the version inside pulp_add_plugin(...VERSION "x.y.z"...)
        std::regex re(R"((pulp_add_plugin\s*\([^)]*VERSION\s+"))" + std::string(old_ver) + R"(")");
        std::string replacement = "$1" + new_ver + "\"";
        auto result = std::regex_replace(content, re, replacement, std::regex_constants::format_first_only);
        if (result == content) return false;  // no match
        content = result;
    } else {
        auto pos = content.find(old_ver);
        if (pos == std::string::npos) return false;
        content.replace(pos, old_ver.size(), new_ver);
    }
    std::ofstream f(cmake_path);
    if (!f) return false;
    f << content;
    return true;
}

struct SemVer {
    int major = 0, minor = 0, patch = 0;

    static SemVer parse(const std::string& s) {
        SemVer v;
        if (sscanf(s.c_str(), "%d.%d.%d", &v.major, &v.minor, &v.patch) < 3)
            v = {};
        return v;
    }

    std::string str() const {
        return std::to_string(major) + "." + std::to_string(minor) + "." + std::to_string(patch);
    }

    SemVer bumped(const std::string& component) const {
        if (component == "major") return {major + 1, 0, 0};
        if (component == "minor") return {major, minor + 1, 0};
        return {major, minor, patch + 1};
    }
};

// ── Commands ────────────────────────────────────────────────────────────────

static int version_show() {
    std::cout << "Pulp SDK version: " << PULP_SDK_VERSION << "\n";

    auto root = find_project_root();
    if (!root.empty()) {
        auto cmake_ver = read_project_cmake_version(root);
        if (!cmake_ver.empty()) {
            std::cout << "Project version:  " << cmake_ver << "\n";
        }
    }
    return 0;
}

static int version_bump(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Usage: pulp version bump <major|minor|patch> [--plugin]\n";
        return 1;
    }

    std::string component = args[0];
    if (component != "major" && component != "minor" && component != "patch") {
        std::cerr << "Error: component must be major, minor, or patch\n";
        return 1;
    }

    bool plugin_mode = false;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--plugin") plugin_mode = true;
    }

    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    auto cmake_path = root / "CMakeLists.txt";
    auto content = read_file_contents(cmake_path);
    if (content.empty()) {
        std::cerr << "Error: could not read " << cmake_path.string() << "\n";
        return 1;
    }

    // Use different regex for --plugin vs SDK version
    std::string current;
    if (plugin_mode) {
        std::regex re(R"(pulp_add_plugin\s*\([^)]*VERSION\s+.(\d+\.\d+\.\d+).)");
        std::smatch m;
        if (std::regex_search(content, m, re)) current = m[1].str();
        if (current.empty()) {
            std::cerr << "Error: no pulp_add_plugin(... VERSION \"x.y.z\" ...) found\n";
            return 1;
        }
    } else {
        current = read_project_cmake_version(root);
        if (current.empty()) {
            std::cerr << "Error: no project(... VERSION x.y.z ...) found\n";
            return 1;
        }
    }

    auto old_ver = SemVer::parse(current);
    auto new_ver = old_ver.bumped(component);
    auto new_str = new_ver.str();

    if (!write_cmake_project_version(cmake_path, current, new_str, plugin_mode)) {
        std::cerr << "Error: failed to update version in " << cmake_path.string() << "\n";
        return 1;
    }

    print_ok("Version bumped: " + current + " -> " + new_str);

    if (!plugin_mode) {
        auto changelog = root / "CHANGELOG.md";
        if (fs::exists(changelog)) {
            auto cl_content = read_file_contents(changelog);
            auto insert_pos = cl_content.find("## [");
            std::string new_entry = "## [" + new_str + "]\n\n";
            if (insert_pos != std::string::npos) {
                cl_content.insert(insert_pos, new_entry);
            } else {
                cl_content = new_entry + cl_content;
            }
            std::ofstream f(changelog);
            if (f) {
                f << cl_content;
                print_ok("Added CHANGELOG.md entry for " + new_str);
            }
        }

        std::cout << "  Note: PULP_SDK_VERSION is derived from CMake project(VERSION)\n";
        std::cout << "        via configure_file — rebuild to pick up the change.\n";
        std::cout << "  Tag with: git tag v" << new_str << "\n";
    }

    return 0;
}

static int version_check() {
    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "Error: not in a Pulp project directory\n";
        return 1;
    }

    bool all_ok = true;
    auto cmake_ver = read_project_cmake_version(root);

    // Check: SDK constant matches CMakeLists.txt
    if (!cmake_ver.empty() && cmake_ver != PULP_SDK_VERSION) {
        print_fail("SDK version mismatch: CMakeLists.txt=" + cmake_ver +
                   " cli=" + std::string(PULP_SDK_VERSION));
        all_ok = false;
    } else if (!cmake_ver.empty()) {
        print_ok("SDK version consistent: " + cmake_ver);
    }

    // Check: AU Info.plist template not hardcoded
    auto au_plist = root / "tools" / "cmake" / "PulpInfoPlist.au.in";
    if (fs::exists(au_plist)) {
        auto content = read_file_contents(au_plist);
        if (content.find("<integer>65536</integer>") != std::string::npos) {
            print_fail("AU Info.plist has hardcoded version integer (65536)");
            all_ok = false;
        } else {
            print_ok("AU Info.plist uses computed version integer");
        }
    }

    // Check: CHANGELOG heading matches
    auto changelog = root / "CHANGELOG.md";
    if (fs::exists(changelog)) {
        auto cl_content = read_file_contents(changelog);
        std::regex heading_re(R"(##\s*\[(\d+\.\d+\.\d+)\])");
        std::smatch m;
        if (std::regex_search(cl_content, m, heading_re)) {
            if (m[1].str() == cmake_ver) {
                print_ok("CHANGELOG latest version matches (" + m[1].str() + ")");
            } else {
                print_warn("CHANGELOG latest (" + m[1].str() + ") differs from CMakeLists.txt (" + cmake_ver + ")");
            }
        }
    }

    return all_ok ? 0 : 1;
}

// ── Entry point ─────────────────────────────────────────────────────────────

int cmd_version(const std::vector<std::string>& args) {
    if (args.empty()) return version_show();

    if (args[0] == "bump") {
        std::vector<std::string> bump_args(args.begin() + 1, args.end());
        return version_bump(bump_args);
    }
    if (args[0] == "check") return version_check();

    std::cerr << "Usage: pulp version [bump <major|minor|patch>] [check]\n";
    return 1;
}
