// cmd_dsp.cpp — `pulp dsp capabilities` subcommand.
//
// Thin shell-out to tools/scripts/dsp_capability_registry.py so the CLI, CI,
// and any agent share ONE implementation and one snapshot. Same split as
// `pulp coverage`: the script owns the logic, this file is dispatcher-only.
//
// The registry answers "what DSP capability surface does this tree expose?" —
// a question that previously had no machine-readable answer anywhere. Forge
// hand-maintained its own registry, the suites hand-maintained their own
// arrays, and they drifted by 21 injectables before a Forge-side contract test
// caught it, one repository downstream and one merge late.

#include "cli_common.hpp"

#include <iostream>

namespace {

void print_dsp_usage() {
    std::cout << "pulp dsp — DSP capability registry\n\n";
    std::cout << "Usage: pulp dsp capabilities [--json | --check | --write]\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  capabilities        Summarise the DSP capability surface\n";
    std::cout << "    --json            Emit the canonical registry to stdout\n";
    std::cout << "    --check           Validate collisions + snapshot freshness\n";
    std::cout << "    --write           Regenerate docs/status/dsp-capabilities.json\n";
    std::cout << "\nThe registry is a STATIC extraction from the Forge bake catalog\n";
    std::cout << "headers. Construction-time values — notably a node's intrinsic\n";
    std::cout << "latency, which is a callback evaluated at the graph's sample rate —\n";
    std::cout << "cannot be read statically and are deliberately absent.\n";
    std::cout << "\n--check fails on a duplicate type id, a duplicate baked-param id\n";
    std::cout << "within a node, or a snapshot that no longer matches the headers.\n";
}

int run_dsp_capabilities(const fs::path& root, const std::vector<std::string>& args) {
    auto script = root / "tools" / "scripts" / "dsp_capability_registry.py";
    if (!fs::exists(script)) {
        std::cerr << "Error: missing " << script.string() << "\n";
        std::cerr << "       Repo predates the DSP capability registry — pull main.\n";
        return 1;
    }
    std::string cmd = "python3 " + shell_quote(script.string());
    for (const auto& arg : args) cmd += " " + shell_quote(arg);
    return run(cmd);
}

}  // namespace

int cmd_dsp(const std::vector<std::string>& args) {
    auto root_opt = require_project_root();
    if (!root_opt) return 1;
    const auto& root = *root_opt;

    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_dsp_usage();
        return 0;
    }

    const std::string& sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "capabilities") {
        return run_dsp_capabilities(root, rest);
    }

    std::cerr << "Error: unknown `pulp dsp` subcommand: " << sub << "\n\n";
    print_dsp_usage();
    return 1;
}
