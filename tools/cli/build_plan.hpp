// SPDX-License-Identifier: MIT
#pragma once

#include "cli_watch.hpp"
#include "focused_build.hpp"
#include "tartci_lease.hpp"

#include <memory>
#include <string>
#include <vector>

struct BuildPlanConfig {
    fs::path project_root;
    fs::path build_dir;
    bool standalone = false;
    bool examples = false;
    bool build_all = false;
    std::vector<std::string> build_args;
    std::string command_kind;
};

// Shared orchestration for the long lived dev/loop build paths. The governor,
// lease policy, target selection and shell command shape remain owned by their
// existing helpers; this type only makes the sequence one reusable plan.
class BuildPlan {
  public:
    explicit BuildPlan(BuildPlanConfig config);

    bool ok() const {
        return lease_.ok();
    }
    int exit_code() const {
        return lease_.exit_code();
    }
    const std::string& error() const {
        return lease_.error();
    }
    bool focus() const {
        return focus_;
    }
    const FocusedSelection& selection() const {
        return selection_;
    }
    const fs::path& project_root() const {
        return config_.project_root;
    }
    const fs::path& build_dir() const {
        return config_.build_dir;
    }
    const CmakeParallelPlan& capped_build() const {
        return capped_build_;
    }
    const TartciAgentBuildLease& lease() const {
        return lease_;
    }

    // Configure through the existing cmd_build entry point when the build tree
    // is absent or incompatible. The flags are intentionally passed through
    // unchanged so bootstrap output and failure classification stay stable.
    int ensure_configured(bool allow_unsupported_sdk);

    // Select affected targets and execute the same initial build used by dev
    // and loop. A failed initial build is reported by the caller so watch mode
    // can retain its existing retry behavior.
    int select_and_build();

    WatchOptions watch_options(bool run_tests, const std::string& test_filter, bool run_validate,
                               const std::string& launch_target,
                               const std::vector<std::string>& launch_args, bool hot_dsp) const;

  private:
    BuildPlanConfig config_;
    TartciAgentBuildLease lease_;
    CmakeParallelPlan capped_build_;
    bool focus_ = false;
    FocusedSelection selection_;
    std::unique_ptr<ScopedBuildParallelEnv> build_env_;
};
