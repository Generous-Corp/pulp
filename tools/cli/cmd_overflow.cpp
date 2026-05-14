// cmd_overflow.cpp — `pulp overflow` subcommand.
//
// Operator surface for the macOS-overflow routing repo variables that
// `.github/workflows/build.yml`'s resolve-provider job reads:
//
//   PULP_LOCAL_MACOS_RUNS_ON_JSON          — selector when local has capacity
//   PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON — selector when local is saturated
//                                            (despite the historical name,
//                                            this is the generic overflow
//                                            target, not Namespace-specific)
//   PULP_LOCAL_MAC_OVERFLOW_THRESHOLD      — BUSY count that trips overflow
//
// Why this lives in `pulp` (not Shipyard): the variable names + semantics are
// Pulp-workflow-specific. Shipyard stays a generic CI orchestrator; per-repo
// CI knobs belong in each repo's own CLI.
//
// Subcommands:
//   pulp overflow status                 — show current routing state
//   pulp overflow enable [--to <sel>]   — set the overflow target var
//   pulp overflow disable                — delete the overflow target var
//   pulp overflow threshold [<N>]        — get or set the BUSY threshold
//
// All commands are thin gh CLI shells. Source of truth = GitHub repo vars.

#include "cli_common.hpp"

#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#  define popen  _popen
#  define pclose _pclose
#endif

namespace {

const std::string kRepo = "danielraffel/pulp";
const std::string kVarLocal = "PULP_LOCAL_MACOS_RUNS_ON_JSON";
const std::string kVarOverflow = "PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON";
const std::string kVarThreshold = "PULP_LOCAL_MAC_OVERFLOW_THRESHOLD";

void print_overflow_usage() {
    std::cout << "pulp overflow — macOS-runner overflow routing\n\n";
    std::cout << "Usage: pulp overflow <subcommand> [options]\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  status              Show current routing (local target, overflow target,\n";
    std::cout << "                      threshold, plus runner-registration state).\n\n";
    std::cout << "  enable [--to JSON]  Set the overflow target. With no --to, defaults to\n";
    std::cout << "                      \"macos-15\" (free GH-hosted). For Namespace, pass\n";
    std::cout << "                      --to '\"namespace-profile-generouscorp-macos\"'.\n\n";
    std::cout << "  disable             Delete the overflow target var. All macOS jobs go\n";
    std::cout << "                      to the local target. In-flight cloud jobs are NOT\n";
    std::cout << "                      cancelled — only new dispatches change.\n\n";
    std::cout << "  threshold [N]       Get (no arg) or set the BUSY count that trips\n";
    std::cout << "                      overflow. Default 2; set to 1 for single-runner\n";
    std::cout << "                      setups so any local job in flight triggers overflow.\n\n";
    std::cout << "Backing repo variables on " << kRepo << ":\n";
    std::cout << "  " << kVarLocal << "\n";
    std::cout << "  " << kVarOverflow << "\n";
    std::cout << "  " << kVarThreshold << "\n";
}

std::string capture(const std::string& cmd) {
    std::string out;
#if defined(_WIN32)
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (!p) return out;
    char buf[1024];
    while (fgets(buf, sizeof(buf), p)) out += buf;
#if defined(_WIN32)
    _pclose(p);
#else
    pclose(p);
#endif
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

std::string read_var(const std::string& name) {
    std::string cmd = "gh api repos/" + kRepo + "/actions/variables/" + name +
                      " --jq .value 2>/dev/null";
    return capture(cmd);
}

int set_var(const std::string& name, const std::string& value) {
    std::string cmd = "gh variable set " + name +
                      " --repo " + kRepo +
                      " --body " + shell_quote(value);
    return run(cmd);
}

int delete_var(const std::string& name) {
    std::string cmd = "gh variable delete " + name +
                      " --repo " + kRepo + " 2>/dev/null";
    return run(cmd);
}

int run_status(const std::vector<std::string>&) {
    auto local = read_var(kVarLocal);
    auto overflow = read_var(kVarOverflow);
    auto threshold = read_var(kVarThreshold);

    std::cout << "macOS routing for " << kRepo << ":\n\n";

    std::cout << "  Local target (idle path):\n";
    std::cout << "    " << kVarLocal << " = "
              << (local.empty() ? "(unset → falls back to GH-hosted macos-15)" : local) << "\n\n";

    std::cout << "  Overflow target (saturated path):\n";
    if (overflow.empty()) {
        std::cout << "    " << kVarOverflow << " = (unset → overflow DISABLED)\n";
        std::cout << "    Every macOS leg goes to the local target above.\n";
    } else {
        std::cout << "    " << kVarOverflow << " = " << overflow << "\n";
    }
    std::cout << "\n";

    std::cout << "  Overflow threshold:\n";
    std::cout << "    " << kVarThreshold << " = "
              << (threshold.empty() ? "2 (default)" : threshold) << "\n";
    std::cout << "    (BUSY count = number of queued+in_progress 'Build and Test' runs\n";
    std::cout << "     excluding the current one; overflow trips when BUSY >= threshold.)\n\n";

    std::cout << "  Self-hosted Mac runner state:\n";
    auto runners = capture(
        "gh api repos/" + kRepo + "/actions/runners "
        "--jq '.runners[] | select(.labels[].name == \"sanitizer\") "
        "| \"    \" + .name + \" (status=\" + .status + (if .busy then \", busy\" else \", idle\" end) + \")\"' "
        "2>/dev/null");
    if (runners.empty()) {
        std::cout << "    (no self-hosted runners with `sanitizer` label registered, OR\n";
        std::cout << "     the default token lacks Administration:Read scope)\n";
    } else {
        std::cout << runners << "\n";
    }

    return 0;
}

int run_enable(const std::vector<std::string>& args) {
    std::string selector = "\"macos-15\"";
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--to" && i + 1 < args.size()) {
            selector = args[++i];
        } else {
            std::cerr << "pulp overflow enable: unknown arg '" << args[i] << "'\n";
            return 1;
        }
    }
    std::cout << "Setting " << kVarOverflow << " = " << selector << "\n";
    int rc = set_var(kVarOverflow, selector);
    if (rc != 0) {
        std::cerr << "pulp overflow enable: failed to set variable\n";
        return rc;
    }
    std::cout << "Overflow enabled. New PR dispatches will route macOS to "
              << selector << " when local is saturated.\n";
    std::cout << "(In-flight workflow_runs keep their original dispatch.)\n";
    return 0;
}

int run_disable(const std::vector<std::string>&) {
    auto current = read_var(kVarOverflow);
    if (current.empty()) {
        std::cout << "Overflow is already disabled (" << kVarOverflow << " not set).\n";
        return 0;
    }
    std::cout << "Deleting " << kVarOverflow << " (was: " << current << ")\n";
    int rc = delete_var(kVarOverflow);
    if (rc != 0) {
        std::cerr << "pulp overflow disable: failed to delete variable\n";
        return rc;
    }
    std::cout << "Overflow disabled. New PR dispatches stay on the local target.\n";
    std::cout << "In-flight cloud jobs continue and complete normally.\n";
    return 0;
}

int run_threshold(const std::vector<std::string>& args) {
    if (args.empty()) {
        auto value = read_var(kVarThreshold);
        std::cout << kVarThreshold << " = "
                  << (value.empty() ? "2 (default)" : value) << "\n";
        return 0;
    }
    if (args.size() > 1) {
        std::cerr << "pulp overflow threshold: too many args\n";
        return 1;
    }
    // Validate integer.
    try {
        int n = std::stoi(args[0]);
        if (n < 0) {
            std::cerr << "pulp overflow threshold: must be >= 0\n";
            return 1;
        }
        std::cout << "Setting " << kVarThreshold << " = " << n << "\n";
        return set_var(kVarThreshold, std::to_string(n));
    } catch (...) {
        std::cerr << "pulp overflow threshold: '" << args[0] << "' is not a number\n";
        return 1;
    }
}

}  // namespace

int cmd_overflow(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_overflow_usage();
        return 0;
    }
    const std::string& sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (sub == "status") return run_status(rest);
    if (sub == "enable") return run_enable(rest);
    if (sub == "disable") return run_disable(rest);
    if (sub == "threshold") return run_threshold(rest);
    std::cerr << "pulp overflow: unknown subcommand '" << sub << "'\n\n";
    print_overflow_usage();
    return 1;
}
