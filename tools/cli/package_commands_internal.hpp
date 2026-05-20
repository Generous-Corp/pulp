// SPDX-License-Identifier: MIT
//
// package_commands_internal.hpp — PRIVATE internal header for the
// `pulp` package-management CLI surface.
//
// Extracted from package_commands.cpp in the 2026-05 roadmap item
// P11-2 mechanical file-split, following the R2-4 (cli_sdk.cpp)
// extraction pattern.
//
// package_commands.cpp historically held every package sub-command
// (search / list / suggest / target / add / remove / update / audit)
// plus the file-local helpers they share. P11-2 splits the
// sub-command clusters into sibling translation units:
//
//   package_commands_util.cpp    — shared print/file/path helpers,
//                                  CMake-block generation, metadata
//                                  (DEPENDENCIES.md / NOTICE.md) edits
//   package_commands_search.cpp  — read-only commands: cmd_search,
//                                  cmd_list, cmd_suggest, cmd_target
//   package_commands_add.cpp     — mutating commands: cmd_add,
//                                  cmd_remove, cmd_update
//   package_commands.cpp         — audit_* entry points (audit lane)
//
// The public sub-command entry points (cmd_*, audit_*) stay declared
// in package_commands.hpp — callers (pulp_cli.cpp) are unchanged.
// This header is PRIVATE: it lives next to the .cpp files under
// tools/cli/ and only relocates the formerly file-local helpers so
// the sibling TUs can share them. Nothing here is part of the public
// include surface.

#pragma once

#include "package_registry.hpp"

#include <string>
#include <vector>

namespace pulp::cli::pkg {

// ── Shared print / colour helpers ──
// (definitions in package_commands_util.cpp; g_color stays a
//  file-local static there — byte-identical to the original)

std::string green(const std::string& s);
std::string red(const std::string& s);
std::string yellow(const std::string& s);
std::string dim(const std::string& s);

void print_ok(const std::string& msg);
void print_fail(const std::string& msg);
void print_warn(const std::string& msg);

// ── Shared argument / file / path helpers ──

bool looks_like_option(const std::string& arg);
bool missing_option_value(const std::vector<std::string>& args, size_t i);

fs::path find_project_root();
fs::path find_registry_path(const fs::path& root);

std::string read_file(const fs::path& path);
bool write_file(const fs::path& path, const std::string& content);

std::string dots(const std::string& name, int width = 35);

// ── CMake generation ──
// (definitions in package_commands_util.cpp; used by cmd_add /
//  cmd_remove / cmd_update)

std::string generate_cmake_block(const PackageDescriptor& pkg,
                                 bool platform_guard = false,
                                 const std::string& guard_platform = "");

std::string generate_packages_cmake(const LockFile& lock, const Registry& reg,
                                     const fs::path& project_root);

void ensure_cmake_include(const fs::path& project_root);

// ── Metadata updates ──
// (definitions in package_commands_util.cpp; used by cmd_add /
//  cmd_remove)

void update_dependencies_md(const fs::path& root, const PackageDescriptor& pkg,
                            bool add);
void update_notice_md(const fs::path& root, const PackageDescriptor& pkg,
                      bool add);

}  // namespace pulp::cli::pkg
