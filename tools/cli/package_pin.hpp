// SPDX-License-Identifier: MIT
//
// package_pin.hpp — immutable source pins for `pulp add` / `pulp update`.
//
// The registry `git_tag` is resolved to a full 40-hex commit (a full commit
// is taken as-is; anything else must be a tag, read from `git ls-remote`
// with the peeled `^{}` line preferred; branches and abbreviated commits are
// refused) and recorded with
//
//   integrity = "sha256-" + sha256(git ls-tree -r -z --full-tree <commit>)
//
// The listing is computed from the commit's tree objects only, so no
// .gitattributes entry (export-subst, export-ignore, eol), checkout filter,
// git configuration, clone depth or git version changes it. The Rust CLI
// (experimental/pulp-rs/src/pkg/pin.rs) computes the identical value, and the
// generated cmake/pulp-packages.cmake re-runs it against the fetched checkout
// after proving HEAD is the locked commit and the working tree is clean.
#pragma once

#include "package_registry.hpp"

#include <filesystem>
#include <functional>
#include <string>

namespace pulp::cli::pkg {

struct SourcePin {
    std::string commit;
    std::string integrity;
    std::string error; // non-empty when pinning failed
};

using SourcePinner =
    std::function<SourcePin(const std::string& repository, const std::string& git_ref)>;

bool is_full_commit(const std::string& s);
bool is_integrity(const std::string& s);
bool is_pinned(const LockedPackage& lp);

// Commit a tag points at in `git ls-remote` output, or "" when absent.
std::string commit_for_tag(const std::string& ls_remote, const std::string& tag);

// Tree-listing integrity of `commit` in the repository at `repo_dir`; "" on
// failure, with the reason in `error` when given.
std::string tree_integrity(const std::filesystem::path& repo_dir, const std::string& commit,
                           std::string* error = nullptr);

// Production pinner: shells out to git.
SourcePin git_pin_source(const std::string& repository, const std::string& git_ref);

// Pin through the active pinner (git unless a test installed another).
SourcePin pin_source(const std::string& repository, const std::string& git_ref);

// Test seam: replace the pinner; an empty function restores git.
void set_source_pinner_for_testing(SourcePinner pinner);

// The CMake verifier emitted at the top of every generated
// cmake/pulp-packages.cmake; byte-identical to
// tools/packages/pulp-packages-verify.cmake (enforced by tests).
extern const char* const kPackagesVerifyCmake;

} // namespace pulp::cli::pkg
