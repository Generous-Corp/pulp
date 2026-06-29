// cmd_coverage.cpp — `pulp coverage diff` subcommand.
//
// Thin shell-out to tools/scripts/local_diff_cover.sh so all four
// invocation surfaces (script, CLI, Claude slash command, pre-push
// hook) share one implementation. The shell script reads the
// threshold + filters from tools/scripts/coverage_config.json, which
// is also the source the CI workflow reads — single source of truth.
//
// The actual diff-coverage logic lives in
// tools/scripts/local_diff_cover.sh (covered end-to-end by
// tools/scripts/test_local_diff_cover.py); this file is dispatcher-only.
// The `cli-coverage-*` ctest shell-out tests still exercise usage,
// unknown-subcommand, and diff-dispatch paths end-to-end.

#include "cli_common.hpp"

#include <iostream>

namespace {

void print_coverage_usage() {
    std::cout << "pulp coverage — local coverage tooling\n\n";
    std::cout << "Usage: pulp coverage <subcommand> [args]\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  diff [TARGET ...]  Run the local diff-coverage check\n";
    std::cout << "                     (mirrors CI's `Diff coverage required` gate).\n";
    std::cout << "                     Optional positional args are passed to\n";
    std::cout << "                     cmake --build ... --target <args> for a\n";
    std::cout << "                     fast targeted build. No args = build all.\n";
    std::cout << "\nThreshold + filters live in tools/scripts/coverage_config.json.\n";
    std::cout << "Set PULP_SKIP_DIFF_COVER=1 to bypass.\n";
}

int run_coverage_diff(const fs::path& root, const std::vector<std::string>& args) {
    auto script = root / "tools" / "scripts" / "local_diff_cover.sh";
    if (!fs::exists(script)) {
        std::cerr << "Error: missing " << script.string() << "\n";
        std::cerr << "       Repo predates the diff-coverage tooling — pull main.\n";
        return 1;
    }
    std::string cmd = "bash " + shell_quote(script);
    for (auto& arg : args) cmd += " " + shell_quote(arg);
    return run(cmd);
}

}  // namespace

int cmd_coverage(const std::vector<std::string>& args) {
    auto root_opt = require_project_root();
    if (!root_opt) return 1;
    const auto& root = *root_opt;

    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_coverage_usage();
        return 0;
    }

    const std::string& sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "diff") {
        return run_coverage_diff(root, rest);
    }

    std::cerr << "Error: unknown `pulp coverage` subcommand: " << sub << "\n\n";
    print_coverage_usage();
    return 1;
}
