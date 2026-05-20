// SPDX-License-Identifier: MIT
//
// package_commands_util.cpp — shared helpers for the `pulp` package
// CLI surface. Extracted from package_commands.cpp in the 2026-05
// roadmap item P11-2 file-split.
//
// This TU holds the formerly file-local helpers that every package
// sub-command cluster depends on: print/colour helpers, argument and
// file/path utilities, CMake-block generation, and the
// DEPENDENCIES.md / NOTICE.md metadata edits. The sub-command bodies
// now live in package_commands_search.cpp / package_commands_add.cpp
// / package_commands.cpp and reach these helpers via the private
// package_commands_internal.hpp header. Code below is moved
// byte-identically from the original package_commands.cpp; the only
// change is dropping `static` from the helpers now shared across TUs
// (their declarations live in package_commands_internal.hpp).

#include "package_commands_internal.hpp"
#include "package_registry.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace pulp::cli::pkg {

// ── Helpers ──

static bool g_color = true;

std::string green(const std::string& s) {
    return g_color ? ("\033[32m" + s + "\033[0m") : s;
}
std::string red(const std::string& s) {
    return g_color ? ("\033[31m" + s + "\033[0m") : s;
}
std::string yellow(const std::string& s) {
    return g_color ? ("\033[33m" + s + "\033[0m") : s;
}
std::string dim(const std::string& s) {
    return g_color ? ("\033[2m" + s + "\033[0m") : s;
}

void print_ok(const std::string& msg) {
    std::cout << green("✓") << " " << msg << "\n";
}
void print_fail(const std::string& msg) {
    std::cerr << red("✗") << " " << msg << "\n";
}
void print_warn(const std::string& msg) {
    std::cout << yellow("⚠") << " " << msg << "\n";
}

bool looks_like_option(const std::string& arg) {
    return arg.starts_with("-");
}

bool missing_option_value(const std::vector<std::string>& args, size_t i) {
    return i + 1 >= args.size() || looks_like_option(args[i + 1]);
}

fs::path find_project_root() {
    auto dir = fs::current_path();
    while (true) {
        if (fs::exists(dir / "CMakeLists.txt") && fs::exists(dir / "core"))
            return dir;
        if (fs::exists(dir / "pulp.toml"))
            return dir;
        auto parent = dir.parent_path();
        if (parent == dir) break;
        dir = parent;
    }
    return {};
}

fs::path find_registry_path(const fs::path& root) {
    auto p = root / "tools" / "packages" / "registry.json";
    if (fs::exists(p)) return p;
    return {};
}

std::string read_file(const fs::path& path) {
    std::ifstream f(path);
    if (!f) return {};
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

bool write_file(const fs::path& path, const std::string& content) {
    if (auto parent = path.parent_path(); !parent.empty())
        fs::create_directories(parent);
    std::ofstream f(path);
    if (!f) return false;
    f << content;
    return f.good();
}

std::string dots(const std::string& name, int width) {
    int n = std::max(1, width - static_cast<int>(name.size()));
    return std::string(n, '.');
}

// ── CMake Generation ──

std::string generate_cmake_block(const PackageDescriptor& pkg,
                                 bool platform_guard,
                                 const std::string& guard_platform) {
    std::ostringstream os;
    os << "# ── " << pkg.id << " (" << pkg.version << ")";
    if (platform_guard) os << " [" << guard_platform << " only]";
    os << " ──\n";

    if (platform_guard) {
        if (guard_platform == "macOS") os << "if(APPLE)\n";
        else if (guard_platform == "Windows") os << "if(WIN32)\n";
        else if (guard_platform == "Linux") os << "if(UNIX AND NOT APPLE)\n";
    }

    std::string indent = platform_guard ? "  " : "";

    if (pkg.fetch.method == "header-only") {
        os << indent << "FetchContent_Declare(" << pkg.id << "\n";
        os << indent << "  GIT_REPOSITORY " << pkg.fetch.git_repository << "\n";
        os << indent << "  GIT_TAG        " << pkg.fetch.git_tag << "\n";
        os << indent << "  GIT_SHALLOW    TRUE\n";
        os << indent << ")\n";
        os << indent << "FetchContent_MakeAvailable(" << pkg.id << ")\n";
        if (!pkg.cmake.targets.empty()) {
            os << indent << "add_library(" << pkg.cmake.targets[0] << " INTERFACE)\n";
            os << indent << "target_include_directories(" << pkg.cmake.targets[0]
               << " INTERFACE ${" << pkg.id << "_SOURCE_DIR}";
            if (!pkg.cmake.include_dir.empty() && pkg.cmake.include_dir != ".")
                os << "/" << pkg.cmake.include_dir;
            os << ")\n";
        }
    } else {
        os << indent << "FetchContent_Declare(" << pkg.id << "\n";
        os << indent << "  GIT_REPOSITORY " << pkg.fetch.git_repository << "\n";
        os << indent << "  GIT_TAG        " << pkg.fetch.git_tag << "\n";
        os << indent << "  GIT_SHALLOW    TRUE\n";
        os << indent << ")\n";
        os << indent << "FetchContent_MakeAvailable(" << pkg.id << ")\n";
    }

    if (platform_guard) {
        auto upper_id = pkg.id;
        std::transform(upper_id.begin(), upper_id.end(), upper_id.begin(),
                       [](unsigned char c) { return c == '-' ? '_' : std::toupper(c); });
        os << "  target_compile_definitions(${PROJECT_NAME} PRIVATE PULP_HAS_"
           << upper_id << "=1)\n";
        os << "endif()\n";
    }

    os << "# ── end " << pkg.id << " ──\n";
    return os.str();
}

std::string generate_packages_cmake(const LockFile& lock, const Registry& reg,
                                    const fs::path& project_root) {
    std::ostringstream os;
    os << "# Auto-generated by pulp package manager — do not edit manually\n";
    os << "# Run 'pulp add <pkg>' or 'pulp remove <pkg>' to modify\n";
    os << "include(FetchContent)\n\n";

    for (auto& [id, lp] : lock.packages) {
        auto it = reg.packages.find(id);
        if (it == reg.packages.end()) continue;
        os << generate_cmake_block(it->second) << "\n";
    }

    return os.str();
}

void ensure_cmake_include(const fs::path& project_root) {
    auto cml = project_root / "CMakeLists.txt";
    auto content = read_file(cml);
    if (content.find("cmake/pulp-packages.cmake") != std::string::npos)
        return;

    std::ofstream f(cml, std::ios::app);
    f << "\n# Pulp package manager\n";
    f << "include(cmake/pulp-packages.cmake OPTIONAL)\n";
}

// ── Metadata Updates ──

void update_dependencies_md(const fs::path& root, const PackageDescriptor& pkg,
                            bool add) {
    auto path = root / "DEPENDENCIES.md";
    auto content = read_file(path);
    if (content.empty()) return;

    std::string entry = "| " + pkg.name + " | " + pkg.version + " | " +
                        pkg.license + " | FetchContent | " + pkg.description + " | 2026-04-07 |";

    if (add) {
        // Insert alphabetically in the table
        std::istringstream stream(content);
        std::ostringstream out;
        std::string line;
        bool inserted = false;
        bool in_table = false;

        while (std::getline(stream, line)) {
            if (line.find("| Name") != std::string::npos || line.find("|---") != std::string::npos) {
                in_table = true;
                out << line << "\n";
                continue;
            }

            if (in_table && !inserted && line.starts_with("| ")) {
                // Extract name from table row for alphabetical comparison
                auto end_name = line.find(" |", 2);
                auto row_name = line.substr(2, end_name - 2);
                // Trim the name
                while (!row_name.empty() && row_name.back() == ' ') row_name.pop_back();

                if (pkg.name < row_name) {
                    out << entry << "\n";
                    inserted = true;
                }
            }

            if (in_table && !line.starts_with("| ") && !inserted) {
                out << entry << "\n";
                inserted = true;
            }

            out << line << "\n";
        }

        if (!inserted) out << entry << "\n";
        write_file(path, out.str());
    } else {
        // Remove: find and delete the line containing this package name
        std::istringstream stream(content);
        std::ostringstream out;
        std::string line;
        while (std::getline(stream, line)) {
            if (line.find("| " + pkg.name + " |") != std::string::npos) continue;
            out << line << "\n";
        }
        write_file(path, out.str());
    }
}

void update_notice_md(const fs::path& root, const PackageDescriptor& pkg,
                      bool add) {
    auto path = root / "NOTICE.md";
    auto content = read_file(path);

    if (add) {
        std::string block = "\n## " + pkg.name + "\n\n"
                          + pkg.license + " — " + pkg.url + "\n";

        // Insert alphabetically by finding the right position
        auto pos = content.find("## ");
        while (pos != std::string::npos) {
            auto end_line = content.find('\n', pos);
            auto section_name = content.substr(pos + 3, end_line - pos - 3);
            if (pkg.name < section_name) {
                content.insert(pos, block + "\n");
                write_file(path, content);
                return;
            }
            pos = content.find("## ", end_line);
        }
        // Append at end
        content += block;
        write_file(path, content);
    } else {
        // Remove the section for this package
        auto header = "## " + pkg.name;
        auto pos = content.find(header);
        if (pos != std::string::npos) {
            auto next = content.find("\n## ", pos + 1);
            if (next == std::string::npos) next = content.size();
            // Also remove leading blank line
            if (pos > 0 && content[pos - 1] == '\n') pos--;
            content.erase(pos, next - pos);
            write_file(path, content);
        }
    }
}

}  // namespace pulp::cli::pkg
