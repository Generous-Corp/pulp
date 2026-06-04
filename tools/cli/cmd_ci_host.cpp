// cmd_ci_host.cpp — `pulp ci-host` subcommand.
//
// Optional discoverability wrapper around tools/ci/setup-ci-host.sh: one-command
// onboarding of a Mac as a Tart-VM CI host (install prereqs, create the local VM
// stores, register the host-class runner label, optionally copy a golden in and
// run a one-shot validation build). This is an advanced/contributor path — never
// required, and Shipyard stays encouraged-not-mandated. See
// docs/guides/mac-ci-host-setup.md and the tart-ci skill.
//
// Subcommands:
//   pulp ci-host setup --class <c> [--copy-from <src>] [--validate] [...]
//       Delegates verbatim to tools/ci/setup-ci-host.sh. Every flag passes
//       through unchanged; this file owns only the discoverable command UX, the
//       real work lives in the script (a thin gh/brew/tart shell, like the other
//       script-backed CLI surfaces). The script needs to resolve from the repo,
//       so the command runs from inside a Pulp checkout (like `pulp ci-local`).

#include "cli_common.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace {

const char* kSetupScript = "tools/ci/setup-ci-host.sh";

void print_ci_host_usage() {
    std::cout << "pulp ci-host — onboard a Mac as a Tart-VM CI host (optional)\n\n";
    std::cout << "Usage: pulp ci-host <subcommand> [options]\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  setup --class <c> [--copy-from <src>] [--validate]\n";
    std::cout << "        Install prereqs, create the VM stores, register the\n";
    std::cout << "        host-class runner label, and (optionally) copy a golden\n";
    std::cout << "        in + run a one-shot validation build. Delegates to\n";
    std::cout << "        " << kSetupScript << ".\n\n";
    std::cout << "Common flags (passed through to the script):\n";
    std::cout << "  --class <name>     REQUIRED host class for the runner label (m5|studio|macbook|...)\n";
    std::cout << "  --copy-from <src>  rsync a golden in from another host/drive (ssh:path | path)\n";
    std::cout << "  --validate         after setup, run one ephemeral VM build to prove it\n";
    std::cout << "  --no-agent         do everything except install/load the launchd agent\n";
    std::cout << "  (run `pulp ci-host setup --help` for the full flag list)\n\n";
    std::cout << "Advanced/contributor path — never required; Shipyard is\n";
    std::cout << "encouraged, not mandated. See docs/guides/mac-ci-host-setup.md\n";
    std::cout << "and the tart-ci skill.\n";
}

int run_setup(const std::vector<std::string>& args) {
    auto root = require_project_root();
    if (!root) return 1;  // require_project_root already printed the reason
    auto script = *root / kSetupScript;
    if (!fs::exists(script)) {
        std::cerr << "pulp ci-host: setup script not found at " << script.string()
                  << "\n";
        return 1;
    }
    std::string cmd = "bash " + shell_quote(script.string());
    for (const auto& arg : args) cmd += " " + shell_quote(arg);
    return run(cmd);
}

}  // namespace

int cmd_ci_host(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_ci_host_usage();
        return 0;
    }

    const std::string& sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "setup") return run_setup(rest);

    std::cerr << "pulp ci-host: unknown subcommand '" << sub << "'\n\n";
    print_ci_host_usage();
    return 1;
}
