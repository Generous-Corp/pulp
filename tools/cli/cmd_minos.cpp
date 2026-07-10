// cmd_minos.cpp — `pulp minos` subcommands (minimum-OS tooling).
//
// Thin shell-out to the two Python tools so the CLI, the `/minos` Claude
// slash command, and the MCP `pulp_minos` tool all share one implementation:
//
//   pulp minos measure <binary>   → tools/scripts/measure_min_os.py --measure
//        Report the minimum OS a single built binary needs, read straight from
//        the artifact (macOS deployment target, Linux glibc symbol version, or
//        Windows PE subsystem). "Derive the floor from everything linked."
//
//   pulp minos sweep [args...]    → tools/scripts/sdk_consumer_sweep.py
//        Rebuild every downstream consumer against one installed SDK and report
//        each project's floor vs the SDK floor. Forwards all args (e.g.
//        --sdk-prefix, --only, --dry-run, --json) to the script.
//
// Dispatcher-only: the real logic + its unit tests live in the scripts. The
// `cli-minos-*` ctest shell-out tests exercise usage / unknown-subcommand /
// dispatch end-to-end.

#include "cli_common.hpp"

#include <iostream>

namespace {

void print_minos_usage() {
    std::cout << "pulp minos — minimum-OS tooling\n\n";
    std::cout << "Usage: pulp minos <subcommand> [args]\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  measure <binary>   Report the minimum OS one built binary needs,\n";
    std::cout << "                     read from the artifact itself (macOS deployment\n";
    std::cout << "                     target / Linux glibc / Windows PE subsystem).\n";
    std::cout << "  sweep [args...]    Rebuild every downstream consumer against one\n";
    std::cout << "                     installed SDK and report each project's floor vs\n";
    std::cout << "                     the SDK floor. Args pass through to the sweep\n";
    std::cout << "                     (e.g. --sdk-prefix <path> --only <repo> --dry-run).\n";
    std::cout << "\nThe floor of any binary is the MAX minimum among everything linked\n";
    std::cout << "into it — the SDK's own libraries, the C++ runtime, and a plugin's\n";
    std::cout << "own code. See docs/guides/minimum-os-support.md.\n";
}

int run_script(const fs::path& root, const std::string& script_rel,
               const std::vector<std::string>& args) {
    auto script = root / "tools" / "scripts" / script_rel;
    if (!fs::exists(script)) {
        std::cerr << "Error: missing " << script.string() << "\n";
        std::cerr << "       Repo predates the minimum-OS tooling — pull main.\n";
        return 1;
    }
    std::string cmd = "python3 " + shell_quote(script);
    for (auto& arg : args) cmd += " " + shell_quote(arg);
    return run(cmd);
}

}  // namespace

int cmd_minos(const std::vector<std::string>& args) {
    auto root_opt = require_project_root();
    if (!root_opt) return 1;
    const auto& root = *root_opt;

    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_minos_usage();
        return 0;
    }

    const std::string& sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "measure") {
        if (rest.empty()) {
            std::cerr << "Error: `pulp minos measure` needs a binary path.\n\n";
            print_minos_usage();
            return 1;
        }
        // measure_min_os.py takes the binary via --measure <path>.
        std::vector<std::string> forwarded{"--measure"};
        forwarded.insert(forwarded.end(), rest.begin(), rest.end());
        return run_script(root, "measure_min_os.py", forwarded);
    }

    if (sub == "sweep") {
        return run_script(root, "sdk_consumer_sweep.py", rest);
    }

    std::cerr << "Error: unknown `pulp minos` subcommand: " << sub << "\n\n";
    print_minos_usage();
    return 1;
}
