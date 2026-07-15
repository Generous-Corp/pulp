// SPDX-License-Identifier: MIT
//
// package_commands_util.cpp — shared helpers for the `pulp` package
// CLI surface.
//
// This TU holds the formerly file-local helpers that every package
// sub-command cluster depends on: print/color helpers, argument and
// file/path utilities, CMake-block generation, and the
// DEPENDENCIES.md / NOTICE.md metadata edits. The sub-command bodies
// now live in package_commands_search.cpp / package_commands_add.cpp
// / package_commands.cpp and reach these helpers via the private
// package_commands_internal.hpp header. Helpers shared across TUs have
// declarations in package_commands_internal.hpp.

#include "package_commands_internal.hpp"
#include "package_registry.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace pulp::cli::pkg {
namespace {

std::string source_dir_expr(const PackageDescriptor& pkg) {
    return "${" + pkg.id + "_SOURCE_DIR}";
}

std::string include_dir_expr(const PackageDescriptor& pkg) {
    auto path = source_dir_expr(pkg);
    if (!pkg.cmake.include_dir.empty() && pkg.cmake.include_dir != ".")
        path += "/" + pkg.cmake.include_dir;
    return path;
}

void emit_fetchcontent(std::ostringstream& os,
                       const PackageDescriptor& pkg,
                       const std::string& indent) {
    os << indent << "FetchContent_Declare(" << pkg.id << "\n";
    os << indent << "  GIT_REPOSITORY " << pkg.fetch.git_repository << "\n";
    os << indent << "  GIT_TAG        " << pkg.fetch.git_tag << "\n";
    os << indent << "  GIT_SHALLOW    TRUE\n";
    if (!pkg.cmake.add_subdirectory)
        os << indent << "  SOURCE_SUBDIR  cmake/pulp-source-only\n";
    os << indent << ")\n";
    os << indent << "FetchContent_MakeAvailable(" << pkg.id << ")\n";
}

void emit_generated_target(std::ostringstream& os,
                           const PackageDescriptor& pkg,
                           const std::string& indent) {
    if (pkg.cmake.targets.empty())
        return;

    const auto& target = pkg.cmake.targets[0];
    if (pkg.cmake.header_only) {
        os << indent << "add_library(" << target << " INTERFACE)\n";
        os << indent << "target_include_directories(" << target
           << " INTERFACE " << include_dir_expr(pkg) << ")\n";
        return;
    }

    if (pkg.cmake.sources.empty())
        return;

    os << indent << "add_library(" << target << " STATIC)\n";
    os << indent << "target_sources(" << target << " PRIVATE\n";
    for (const auto& source : pkg.cmake.sources)
        os << indent << "  " << source_dir_expr(pkg) << "/" << source << "\n";
    os << indent << ")\n";
    os << indent << "target_include_directories(" << target
       << " PUBLIC " << include_dir_expr(pkg) << ")\n";
    os << indent << "set_target_properties(" << target
       << " PROPERTIES POSITION_INDEPENDENT_CODE ON)\n";
    os << indent << "if(CMAKE_DL_LIBS)\n";
    os << indent << "  target_link_libraries(" << target
       << " PUBLIC ${CMAKE_DL_LIBS})\n";
    os << indent << "endif()\n";
}

std::string feature_macro_name(std::string id) {
    std::transform(id.begin(), id.end(), id.begin(),
                   [](unsigned char c) { return c == '-' ? '_' : std::toupper(c); });
    return id;
}

}  // namespace

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

    emit_fetchcontent(os, pkg, indent);
    emit_generated_target(os, pkg, indent);

    if (platform_guard) {
        os << "  target_compile_definitions(${PROJECT_NAME} PRIVATE PULP_HAS_"
           << feature_macro_name(pkg.id) << "=1)\n";
        os << "endif()\n";
    }

    os << "# ── end " << pkg.id << " ──\n";
    return os.str();
}

std::string generate_cmake_block_for_condition(const PackageDescriptor& pkg,
                                               const std::string& guard_condition) {
    std::ostringstream os;
    os << "# ── " << pkg.id << " (" << pkg.version << ") [platform guard] ──\n";
    os << "if(" << guard_condition << ")\n";
    emit_fetchcontent(os, pkg, "  ");
    emit_generated_target(os, pkg, "  ");
    os << "  target_compile_definitions(${PROJECT_NAME} PRIVATE PULP_HAS_"
       << feature_macro_name(pkg.id) << "=1)\n";
    os << "endif()\n";
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
