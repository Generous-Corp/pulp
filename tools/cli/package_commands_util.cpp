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
#include "tool_registry.hpp"

#include <pulp/platform/child_process.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
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

void emit_fetchcontent(std::ostringstream& os, const PackageDescriptor& pkg,
                       const LockedPackage& locked, const std::string& indent) {
    if (is_pinned(locked)) {
        const auto& repository =
            locked.resolved.empty() ? pkg.fetch.git_repository : locked.resolved;
        os << indent << "if(COMMAND pulp_register_fetchcontent_source)\n";
        os << indent << "  pulp_register_fetchcontent_source(" << pkg.id << " REF " << locked.commit
           << ")\n";
        os << indent << "endif()\n";
        os << indent << "FetchContent_Declare(" << pkg.id << "\n";
        os << indent << "  GIT_REPOSITORY " << repository << "\n";
        os << indent << "  GIT_TAG        " << locked.commit << "\n";
        os << indent << "  SOURCE_SUBDIR  pulp-package-populate-only\n";
        os << indent << ")\n";
        os << indent << "_pulp_package_make_available(" << pkg.id << " " << locked.commit << " "
           << locked.integrity << " " << (pkg.cmake.add_subdirectory ? "ON" : "OFF") << ")\n";
        return;
    }
    os << indent << "message(WARNING \"Pulp package " << pkg.id
       << " is not pinned in packages.lock.json (no full commit + integrity); it is "
          "fetched by mutable tag. Run 'pulp update --apply' to pin it.\")\n";
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

// ── Source pinning ──

namespace {

SourcePinner& active_pinner() {
    static SourcePinner pinner;
    return pinner;
}

bool is_lower_hex(const std::string& s) {
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

struct GitResult {
    bool ok = false;
    std::string out;
    std::string err;
};

GitResult run_git(const fs::path& dir, const std::vector<std::string>& args) {
    std::vector<std::string> argv;
    if (!dir.empty()) {
        argv.push_back("-C");
        argv.push_back(dir.string());
    }
    argv.insert(argv.end(), args.begin(), args.end());
    pulp::platform::ProcessOptions opts;
    opts.timeout_ms = 600000;
    opts.max_output_bytes = 8u << 20;
    auto r = pulp::platform::ChildProcess::run("git", argv, opts);
    GitResult g;
    g.ok = r.exit_code == 0 && !r.timed_out;
    g.out = r.stdout_output;
    g.err = r.stderr_output;
    while (!g.err.empty() && (g.err.back() == '\n' || g.err.back() == '\r'))
        g.err.pop_back();
    return g;
}

struct ScratchDir {
    fs::path path;
    ScratchDir() {
        static std::atomic<unsigned> seq{0};
        std::error_code ec;
        auto base = fs::temp_directory_path(ec);
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            auto candidate =
                base /
                ("pulp-pkg-pin-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(seq.fetch_add(1)));
            if (fs::create_directory(candidate, ec)) {
                path = candidate;
                return;
            }
        }
    }
    ~ScratchDir() {
        std::error_code ec;
        if (!path.empty())
            fs::remove_all(path, ec);
    }
};

} // namespace

bool is_full_commit(const std::string& s) {
    return s.size() == 40 && is_lower_hex(s);
}

bool is_integrity(const std::string& s) {
    static const std::string prefix = "sha256-";
    if (s.rfind(prefix, 0) != 0)
        return false;
    auto hex = s.substr(prefix.size());
    return hex.size() == 64 && is_lower_hex(hex);
}

bool is_pinned(const LockedPackage& lp) {
    return is_full_commit(lp.commit) && is_integrity(lp.integrity);
}

std::string commit_for_tag(const std::string& ls_remote, const std::string& tag) {
    const std::string plain = "refs/tags/" + tag;
    const std::string peeled = plain + "^{}";
    std::string plain_hit;
    std::istringstream lines(ls_remote);
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string sha, name;
        if (!(fields >> sha >> name))
            continue;
        std::transform(sha.begin(), sha.end(), sha.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!is_full_commit(sha))
            continue;
        if (name == peeled)
            return sha;
        if (name == plain)
            plain_hit = sha;
    }
    return plain_hit;
}

std::string archive_integrity(const fs::path& repo_dir, const std::string& commit,
                              std::string* error) {
    ScratchDir work;
    if (work.path.empty()) {
        if (error)
            *error = "could not create a scratch directory";
        return {};
    }
    auto tar = work.path / "source.tar";
    auto r =
        run_git(repo_dir, {"-c", "core.autocrlf=false", "-c", "core.eol=lf", "-c", "tar.umask=0002",
                           "archive", "--format=tar", "-o", tar.string(), commit});
    if (!r.ok) {
        if (error)
            *error = "git archive failed: " + r.err;
        return {};
    }
    auto hex = pulp::cli::tools::sha256_file_hex(tar);
    if (hex.size() != 64) {
        if (error)
            *error = "could not hash the source archive";
        return {};
    }
    return "sha256-" + hex;
}

SourcePin git_pin_source(const std::string& repository, const std::string& git_ref) {
    SourcePin pin;
    std::string commit = git_ref;
    if (!is_full_commit(git_ref)) {
        // No ref pattern: `ls-remote <url> <tag>` omits the peeled `^{}`
        // line, which would pin an annotated tag object instead of its commit.
        auto ls = run_git({}, {"ls-remote", "--tags", repository});
        if (!ls.ok) {
            pin.error = "git ls-remote failed: " + ls.err;
            return pin;
        }
        commit = commit_for_tag(ls.out, git_ref);
        if (commit.empty()) {
            pin.error = "'" + git_ref + "' is not a tag of " + repository +
                        "; registry pins must be a release tag or a full 40-hex commit";
            return pin;
        }
    }

    ScratchDir work;
    if (work.path.empty()) {
        pin.error = "could not create a scratch directory";
        return pin;
    }
    if (auto r = run_git(work.path, {"init", "-q"}); !r.ok) {
        pin.error = "git init failed: " + r.err;
        return pin;
    }
    if (!run_git(work.path, {"fetch", "-q", "--depth", "1", repository, commit}).ok) {
        // Some servers refuse a fetch by commit id; fall back to the tag,
        // then to every head and tag.
        bool fetched = !is_full_commit(git_ref) &&
                       run_git(work.path, {"fetch", "-q", repository, "refs/tags/" + git_ref}).ok;
        if (!fetched) {
            auto all = run_git(work.path,
                               {"fetch", "-q", repository, "+refs/heads/*:refs/remotes/origin/*",
                                "+refs/tags/*:refs/tags/*"});
            if (!all.ok) {
                pin.error = "git fetch failed: " + all.err;
                return pin;
            }
        }
    }
    auto resolved = run_git(work.path, {"rev-parse", "--verify", "--quiet", commit + "^{commit}"});
    std::string resolved_sha = resolved.out.substr(0, resolved.out.find_first_of("\r\n"));
    if (!resolved.ok || resolved_sha != commit) {
        pin.error = repository + " does not contain commit " + commit;
        return pin;
    }
    std::string err;
    pin.integrity = archive_integrity(work.path, commit, &err);
    if (pin.integrity.empty()) {
        pin.error = err;
        return pin;
    }
    pin.commit = commit;
    return pin;
}

SourcePin pin_source(const std::string& repository, const std::string& git_ref) {
    if (auto& pinner = active_pinner(); pinner)
        return pinner(repository, git_ref);
    return git_pin_source(repository, git_ref);
}

void set_source_pinner_for_testing(SourcePinner pinner) {
    active_pinner() = std::move(pinner);
}

const char* const kPackagesVerifyCmake = R"PULPCMAKE(find_package(Git QUIET)

# Each package below is fetched at the full commit recorded in
# packages.lock.json and checked against the lock's integrity before it is
# added to the build. integrity = "sha256-" + sha256 of
#   git -c core.autocrlf=false -c core.eol=lf -c tar.umask=0002 \
#       archive --format=tar <commit>
# A mismatch stops the configure. FETCHCONTENT_FULLY_DISCONNECTED and
# FETCHCONTENT_SOURCE_DIR_<NAME> are honoured by FetchContent itself; the
# check needs no network.
function(_pulp_package_verify name commit integrity)
  string(TOLOWER "${name}" _lc)
  FetchContent_GetProperties(${name})
  set(_src "${${_lc}_SOURCE_DIR}")
  set(_stamp "${commit} ${integrity} ${_src}")
  if("${_PULP_PACKAGE_VERIFIED_${_lc}}" STREQUAL "${_stamp}")
    return()
  endif()
  if(NOT EXISTS "${_src}/.git")
    message(WARNING "Pulp package ${name}: ${_src} is not a git checkout, so "
      "its locked integrity (${integrity}) cannot be verified. This is "
      "expected only for an explicit source override.")
    return()
  endif()
  if(NOT GIT_EXECUTABLE)
    message(FATAL_ERROR "Pulp package ${name}: git is required to verify "
      "the integrity recorded in packages.lock.json")
  endif()
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_src}" rev-parse --verify --quiet "${commit}^{commit}"
    RESULT_VARIABLE _rc OUTPUT_VARIABLE _resolved
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT _rc EQUAL 0 OR NOT "${_resolved}" STREQUAL "${commit}")
    message(FATAL_ERROR "Pulp package ${name}: locked commit ${commit} is "
      "not present in ${_src}")
  endif()
  set(_dir "${CMAKE_BINARY_DIR}/pulp-packages")
  file(MAKE_DIRECTORY "${_dir}")
  set(_tar "${_dir}/${_lc}-${commit}.tar")
  execute_process(
    COMMAND "${GIT_EXECUTABLE}" -C "${_src}" -c core.autocrlf=false -c core.eol=lf
            -c tar.umask=0002 archive --format=tar -o "${_tar}" "${commit}"
    RESULT_VARIABLE _rc ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "Pulp package ${name}: git archive failed: ${_err}")
  endif()
  file(SHA256 "${_tar}" _digest)
  file(REMOVE "${_tar}")
  if(NOT "sha256-${_digest}" STREQUAL "${integrity}")
    message(FATAL_ERROR "Pulp package ${name}: integrity mismatch for ${commit}\n"
      "  locked:   ${integrity}\n"
      "  computed: sha256-${_digest}\n"
      "Refusing to build an unverified dependency. If the pin changed on "
      "purpose, run 'pulp update --apply' to re-pin it.")
  endif()
  set(_PULP_PACKAGE_VERIFIED_${_lc} "${_stamp}" CACHE INTERNAL
    "Verified Pulp package pin for ${name}")
endfunction()

# Populate (the declared SOURCE_SUBDIR never exists, so FetchContent does not
# add the package yet), verify, then add the package's own CMake project.
macro(_pulp_package_make_available name commit integrity add_subdir)
  FetchContent_MakeAvailable(${name})
  _pulp_package_verify(${name} ${commit} ${integrity})
  string(TOLOWER "${name}" _pulp_package_lc)
  if(${add_subdir} AND EXISTS "${${_pulp_package_lc}_SOURCE_DIR}/CMakeLists.txt")
    add_subdirectory("${${_pulp_package_lc}_SOURCE_DIR}"
                     "${${_pulp_package_lc}_BINARY_DIR}")
  endif()
endmacro()
)PULPCMAKE";

// ── CMake Generation ──

std::string generate_cmake_block(const PackageDescriptor& pkg, const LockedPackage& locked,
                                 bool platform_guard, const std::string& guard_platform) {
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

    emit_fetchcontent(os, pkg, locked, indent);
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
                                               const LockedPackage& locked,
                                               const std::string& guard_condition) {
    std::ostringstream os;
    os << "# ── " << pkg.id << " (" << pkg.version << ") [platform guard] ──\n";
    os << "if(" << guard_condition << ")\n";
    emit_fetchcontent(os, pkg, locked, "  ");
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
    os << "include(FetchContent)\n";
    os << kPackagesVerifyCmake << "\n";

    for (auto& [id, lp] : lock.packages) {
        auto it = reg.packages.find(id);
        if (it == reg.packages.end()) continue;
        os << generate_cmake_block(it->second, lp) << "\n";
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
