// SPDX-License-Identifier: MIT
#include "build_plan.hpp"

#include "cli_common.hpp"
#include "shell_quote.hpp"

#include <iostream>

BuildPlan::BuildPlan(BuildPlanConfig config) : config_(std::move(config)) {
    auto lease_kind = config_.command_kind;
    for (auto& c : lease_kind)
        if (c == ' ')
            c = '-';
    lease_ = TartciAgentBuildLease::acquire({config_.project_root, lease_kind, true});
    if (!lease_.ok())
        return;
    build_env_ = std::make_unique<ScopedBuildParallelEnv>(lease_.jobs(), lease_.active());
    capped_build_ = cap_cmake_build_parallel_args(config_.build_args, lease_.jobs());
    migrate_slow_build_dir(config_.build_dir, !config_.standalone, config_.examples);
}

int BuildPlan::ensure_configured(bool allow_unsupported_sdk) {
    if (!enforce_project_cli_compatibility(config_.project_root, config_.command_kind,
                                           allow_unsupported_sdk)) {
        return 1;
    }
    const bool missing =
        !fs::exists(config_.build_dir / "CMakeCache.txt") ||
        (!config_.standalone && !source_checkout_dependencies_enabled(
                                    config_.project_root, config_.build_dir / "CMakeCache.txt")) ||
        (config_.examples && !config_.standalone && build_dir_has_examples_off(config_.build_dir));
    if (!missing)
        return 0;

    std::cout << "Project not configured. "
              << (config_.command_kind == "pulp loop" ? "Configuring + building first...\n"
                                                      : "Building first...\n");
    std::vector<std::string> bootstrap_args;
    if (allow_unsupported_sdk)
        bootstrap_args.push_back("--allow-unsupported-sdk");
    if (config_.examples)
        bootstrap_args.push_back("--examples");
    if (config_.build_all || build_args_name_target(config_.build_args)) {
        bootstrap_args.push_back("--all");
    }
    return cmd_build(bootstrap_args);
}

int BuildPlan::select_and_build() {
    focus_ = focused_build_applicable(config_.project_root, config_.standalone, config_.build_args,
                                      config_.build_all);
    if (focus_) {
        const auto rc = ensure_codemodel_reply(config_.project_root, config_.build_dir,
                                               !config_.standalone, config_.examples);
        if (rc != 0)
            return rc;
    }
    selection_ = select_for_rebuild(config_.project_root, config_.build_dir, focus_, "");

    std::string build_cmd = "cmake --build " + config_.build_dir.string();
    for (const auto& arg : capped_build_.args)
        build_cmd += " " + arg;
    build_cmd = focused_build_command(build_cmd, selection_);
    if (focused_nothing_to_build(selection_))
        return 0;
    return run_with_spinner(
        apply_build_dir_lock(apply_agent_build_watchdog(
                                 apply_agent_build_qos(build_cmd, lease_.qos(), lease_.floor()),
                                 lease_.jobs(), lease_.active()),
                             config_.project_root, config_.build_dir),
        "Building");
}

WatchOptions BuildPlan::watch_options(bool run_tests, const std::string& test_filter,
                                      bool run_validate, const std::string& launch_target,
                                      const std::vector<std::string>& launch_args,
                                      bool hot_dsp) const {
    WatchOptions opts;
    opts.root = config_.project_root;
    opts.build_dir = config_.build_dir;
    opts.build_args = capped_build_.args;
    opts.run_tests = run_tests;
    opts.test_filter = test_filter;
    opts.run_validate = run_validate;
    opts.focus = focus_;
    opts.launch_target = launch_target;
    opts.launch_args = launch_args;
    opts.hot_dsp = hot_dsp;
    opts.build_jobs = capped_build_.jobs;
    opts.build_qos = lease_.qos();
    opts.build_watchdog = lease_.active();
    return opts;
}
