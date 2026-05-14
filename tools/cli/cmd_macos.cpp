// cmd_macos.cpp — `pulp macos` subcommand.
//
// Operator surface for retargeting JUST the macOS leg of a PR's CI to a
// different runner pool, without disturbing the matrix-bound Linux/Windows
// legs. Backed by .github/workflows/build-macos.yml (see pulp task #20).
//
// Subcommands:
//   pulp macos retarget --pr N --to <local|namespace|github-hosted>
//       Cancels any in-flight macOS dispatch for PR N and fires a fresh
//       workflow_dispatch on build-macos.yml against the PR's head branch
//       with the chosen runner. Branch protection's `macos` required-check
//       is satisfied by whichever workflow most recently produced that
//       check name, so retargeting supersedes the previous macOS leg.
//
//   pulp macos status [--pr N]
//       Reports the current macOS routing decision for the PR (or the
//       current branch's PR if no --pr). Prints which runner pool the most
//       recent macOS dispatch landed on and its status.
//
// Both subcommands are thin gh-API shells. The real work is in
// build-macos.yml — this file just owns the operator UX.

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

const std::string kBuildMacosWorkflow = "build-macos.yml";
const std::string kRepo = "danielraffel/pulp";

void print_macos_usage() {
    std::cout << "pulp macos — per-PR macOS-runner retargeting\n\n";
    std::cout << "Usage: pulp macos <subcommand> [options]\n\n";
    std::cout << "Subcommands:\n";
    std::cout << "  retarget --pr <N> --to <runner>   Cancel current macOS dispatch,\n";
    std::cout << "                                    re-fire on the chosen runner.\n";
    std::cout << "                                    runner = local | namespace | github-hosted\n\n";
    std::cout << "  status [--pr <N>]                 Show where the latest macOS\n";
    std::cout << "                                    dispatch is routed and its state.\n\n";
    std::cout << "Repo variables consulted:\n";
    std::cout << "  PULP_LOCAL_MACOS_RUNS_ON_JSON          — selector for `--to local`\n";
    std::cout << "  PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON — selector for `--to namespace`\n";
    std::cout << "  (`--to github-hosted` always resolves to \"macos-15\")\n\n";
    std::cout << "See .github/workflows/build-macos.yml for the underlying workflow.\n";
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
    // Trim trailing whitespace.
    while (!out.empty() && (out.back() == '\n' || out.back() == ' ' || out.back() == '\t')) {
        out.pop_back();
    }
    return out;
}

std::optional<std::string> pr_head_ref(const std::string& pr_number) {
    std::string cmd = "gh api repos/" + kRepo + "/pulls/" + pr_number +
                      " --jq .head.ref 2>/dev/null";
    auto out = capture(cmd);
    if (out.empty()) return std::nullopt;
    return out;
}

int cancel_in_flight_macos(const std::string& pr_number) {
    // Find any in-flight or queued workflow_runs (from either build.yml or
    // build-macos.yml) where the macOS job hasn't completed yet, and cancel
    // them so the new dispatch isn't shadowed by a stale one.
    std::string head_ref;
    {
        auto opt = pr_head_ref(pr_number);
        if (!opt) {
            std::cerr << "pulp macos: cannot resolve head ref for PR #" << pr_number << "\n";
            return 1;
        }
        head_ref = *opt;
    }

    std::string list_cmd =
        "gh api \"repos/" + kRepo + "/actions/runs?event=pull_request&per_page=100\" "
        "--jq '.workflow_runs[] | select(.head_branch == \"" + head_ref + "\") "
        "| select(.name == \"Build and Test\" or .name == \"Build and Test (macOS retarget)\") "
        "| select(.status == \"queued\" or .status == \"in_progress\") | .id'";

    auto raw_ids = capture(list_cmd);
    if (raw_ids.empty()) {
        std::cout << "pulp macos: no in-flight macOS-bearing runs to cancel for PR #"
                  << pr_number << "\n";
        return 0;
    }

    std::istringstream stream(raw_ids);
    std::string id;
    int cancelled = 0;
    while (std::getline(stream, id)) {
        if (id.empty()) continue;
        std::string cancel_cmd =
            "gh api -X POST repos/" + kRepo + "/actions/runs/" + id +
            "/cancel >/dev/null 2>&1";
        if (std::system(cancel_cmd.c_str()) == 0) {
            std::cout << "pulp macos: cancelled run " << id << "\n";
            ++cancelled;
        } else {
            std::cerr << "pulp macos: failed to cancel run " << id << " (continuing)\n";
        }
    }
    std::cout << "pulp macos: cancelled " << cancelled << " run(s) for PR #"
              << pr_number << "\n";
    return 0;
}

int dispatch_retarget(const std::string& pr_number, const std::string& runner) {
    auto head_ref_opt = pr_head_ref(pr_number);
    if (!head_ref_opt) {
        std::cerr << "pulp macos: cannot resolve head ref for PR #" << pr_number << "\n";
        return 1;
    }
    const auto& head_ref = *head_ref_opt;

    std::string cmd = "gh workflow run " + kBuildMacosWorkflow +
                      " --repo " + kRepo +
                      " --ref " + shell_quote(head_ref) +
                      " --field runner=" + shell_quote(runner) +
                      " --field target_ref=" + shell_quote(head_ref);

    int rc = run(cmd);
    if (rc != 0) {
        std::cerr << "pulp macos: workflow dispatch failed (exit " << rc << ")\n";
        return rc;
    }
    std::cout << "pulp macos: dispatched build-macos.yml on '" << head_ref
              << "' with runner=" << runner << "\n";
    std::cout << "  View progress: gh run watch --repo " << kRepo << "\n";
    return 0;
}

int run_retarget(const std::vector<std::string>& args) {
    std::string pr_number;
    std::string runner;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--pr" && i + 1 < args.size()) {
            pr_number = args[++i];
        } else if (args[i] == "--to" && i + 1 < args.size()) {
            runner = args[++i];
        } else {
            std::cerr << "pulp macos retarget: unknown arg '" << args[i] << "'\n";
            return 1;
        }
    }
    if (pr_number.empty() || runner.empty()) {
        std::cerr << "Usage: pulp macos retarget --pr <N> --to <local|namespace|github-hosted>\n";
        return 1;
    }
    if (runner != "local" && runner != "namespace" && runner != "github-hosted") {
        std::cerr << "pulp macos retarget: --to must be one of: local, namespace, github-hosted\n";
        return 1;
    }

    int rc = cancel_in_flight_macos(pr_number);
    if (rc != 0) return rc;
    return dispatch_retarget(pr_number, runner);
}

int run_status(const std::vector<std::string>& args) {
    std::string pr_number;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--pr" && i + 1 < args.size()) {
            pr_number = args[++i];
        } else {
            std::cerr << "pulp macos status: unknown arg '" << args[i] << "'\n";
            return 1;
        }
    }
    if (pr_number.empty()) {
        // Try to resolve from current branch.
        auto branch = capture("git rev-parse --abbrev-ref HEAD 2>/dev/null");
        if (branch.empty()) {
            std::cerr << "pulp macos status: --pr required (current branch unknown)\n";
            return 1;
        }
        std::string lookup =
            "gh api \"repos/" + kRepo + "/pulls?head=danielraffel:" + branch +
            "&state=open&per_page=1\" --jq '.[0].number // empty' 2>/dev/null";
        pr_number = capture(lookup);
        if (pr_number.empty()) {
            std::cerr << "pulp macos status: no open PR found for branch '"
                      << branch << "'; pass --pr explicitly\n";
            return 1;
        }
    }

    std::string head_cmd =
        "gh api \"repos/" + kRepo + "/pulls/" + pr_number + "\" --jq .head.sha 2>/dev/null";
    auto sha = capture(head_cmd);
    if (sha.empty()) {
        std::cerr << "pulp macos status: cannot resolve head SHA for PR #" << pr_number << "\n";
        return 1;
    }

    std::string lookup =
        "gh api \"repos/" + kRepo + "/commits/" + sha + "/check-runs?per_page=100\" "
        "--jq '[.check_runs[] | select(.name == \"macos\")] "
        "| sort_by(.started_at) | reverse | .[0] "
        "| {name, status, conclusion, html_url}'";

    std::cout << "PR #" << pr_number << " — latest `macos` check:\n";
    return run(lookup);
}

}  // namespace

int cmd_macos(const std::vector<std::string>& args) {
    if (args.empty() || args[0] == "--help" || args[0] == "-h" || args[0] == "help") {
        print_macos_usage();
        return 0;
    }

    const std::string& sub = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());

    if (sub == "retarget") return run_retarget(rest);
    if (sub == "status") return run_status(rest);

    std::cerr << "pulp macos: unknown subcommand '" << sub << "'\n\n";
    print_macos_usage();
    return 1;
}
