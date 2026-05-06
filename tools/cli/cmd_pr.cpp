// cmd_pr.cpp — `pulp pr` subcommand.
//
// Primary behavior: delegate to `shipyard pr` when the effective PR
// workflow is `shipyard` (the default) and Shipyard is installed.
// Shipyard is Pulp's single-source-of-truth ship orchestrator; keeping
// a parallel native implementation in two tools is how drift starts.
// See issue #352.
//
// If Shipyard is unavailable while the shipyard workflow is selected,
// print a concise install/switch guide and exit 2. `--workflow github`
// selects the explicit GitHub CLI (`gh`) path. `--workflow manual`
// prints the intended manual steps and exits without mutating PR state.
// `--native` forces the in-CLI implementation below, which stays as a
// diagnostic fallback when shipyard itself is broken or under debug.
//
// The in-CLI fallback does the same 4 steps shipyard pr does:
//
//   1. Skill-sync gate — hard-fails if a mapped path is touched without
//      updating the corresponding SKILL.md (or a Skill-Update trailer on
//      the tip commit). Only step that can't be automated: skill content
//      requires human judgment.
//   2. Version-bump apply — rewrites CMakeLists.txt project(VERSION) and
//      .claude-plugin/{plugin,marketplace}.json, then stages them and
//      emits a `chore: bump <surfaces>` commit.
//   3. PR + validate + merge — runs `gh pr create` with a generated body
//      describing the bumps, then `shipyard ship` for the cross-platform
//      validation and merge on green.
//
// The command never guesses whether to push — the human (or ralph loop)
// decides that. It never auto-tags either; auto-release runs on push to
// main via .github/workflows/auto-release.yml.
//
// Natural-language triggers ("push a PR", "ship this", "we're done") are
// routed here via the ci skill and CLAUDE.md's Versioning & Skill-Sync
// Policy section. Agents should never invoke gh/shipyard/the Python
// scripts directly.

#include "cli_common.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#  include <sys/wait.h>
#else
// Windows MSVC ships POSIX-named shims under leading-underscore names:
// _popen / _pclose live in <stdio.h>. Map the POSIX spellings onto them
// so the TU below compiles unchanged on both toolchains. This was the
// root cause of every release-cli.yml run v0.4.0..v0.13.0 failing on
// the CLI windows-x64 and CLI windows-arm64 jobs.
#  include <stdio.h>
#  define popen  _popen
#  define pclose _pclose
#  ifndef WIFEXITED
#    define WIFEXITED(status)   (true)
#  endif
#  ifndef WEXITSTATUS
#    define WEXITSTATUS(status) (status)
#  endif
#endif

namespace {

// ── Process plumbing ────────────────────────────────────────────────────

struct CommandResult {
    int         exit_code;
    std::string stdout_text;
    std::string stderr_text;
};

// Run a command, capture stdout+stderr separately. Uses popen piped
// through /bin/sh so PATH resolution is consistent with an interactive
// shell. stderr goes through a temp-file hack since popen only exposes
// stdout; acceptable here because pulp pr is main-thread, not perf-sensitive.
CommandResult run_capture(const std::string& cmd) {
    // Redirect stderr to a marker delimiter we strip later.
    // Simpler: merge 2>&1 and annotate. Callers who need them separate
    // can parse by heuristics; we keep it combined.
    CommandResult out{0, {}, {}};
    std::string full = cmd + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) {
        out.exit_code = -1;
        out.stderr_text = "popen failed";
        return out;
    }
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), pipe)) {
        out.stdout_text += buf;
    }
    int st = pclose(pipe);
    // pclose returns the wait(2)-style status on success, -1 on error.
    if (st == -1) {
        out.exit_code = -1;
    } else if (WIFEXITED(st)) {
        out.exit_code = WEXITSTATUS(st);
    } else {
        out.exit_code = 1;
    }
    return out;
}

int run_passthrough(const std::string& cmd) {
    // Stream child output directly to the user's terminal.
    int st = std::system(cmd.c_str());
    if (st == -1) return -1;
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 1;
}

// ── Helpers ─────────────────────────────────────────────────────────────

// Local trim — defined as static so it doesn't collide with the
// equivalent non-static definition in cli_common.cpp. Both have the same
// behavior; we keep the local one to avoid forcing a header rebuild.
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string current_branch(const fs::path& root) {
    auto r = run_capture("git -C '" + root.string() + "' rev-parse --abbrev-ref HEAD");
    if (r.exit_code != 0) return {};
    return trim(r.stdout_text);
}

bool has_clean_worktree(const fs::path& root) {
    auto r = run_capture("git -C '" + root.string() + "' status --porcelain");
    return r.exit_code == 0 && trim(r.stdout_text).empty();
}

bool push_upstream(const fs::path& root, const std::string& branch) {
    std::cout << color::dim() << "Pushing " << branch << " to origin..." << color::reset() << "\n";
    return run_passthrough("git -C '" + root.string() + "' push -u origin " + branch) == 0;
}

// ── Subcommand steps ────────────────────────────────────────────────────

int step_skill_sync(const fs::path& root, const std::string& base) {
    std::cout << color::bold() << "▸ Skill-sync check" << color::reset() << "\n";
    std::string cmd = "python3 '" + (root / "tools/scripts/skill_sync_check.py").string()
                    + "' --base '" + base + "' --mode=report";
    return run_passthrough(cmd);
}

int step_version_bump(const fs::path& root, const std::string& base) {
    std::cout << color::bold() << "▸ Version-bump apply" << color::reset() << "\n";
    std::string cmd = "python3 '" + (root / "tools/scripts/version_bump_check.py").string()
                    + "' --base '" + base + "' --mode=apply";
    return run_passthrough(cmd);
}

int step_commit_bumps(const fs::path& root) {
    // Only emit a commit if the apply step actually staged changes.
    auto staged = run_capture("git -C '" + root.string() + "' diff --cached --name-only");
    if (trim(staged.stdout_text).empty()) {
        std::cout << color::dim() << "▸ No version bump required — skipping chore commit." << color::reset() << "\n";
        return 0;
    }
    std::cout << color::bold() << "▸ Committing version bump(s)" << color::reset() << "\n";

    // Derive a commit message from the changed files.
    std::ostringstream msg;
    msg << "chore: bump versions\n\nAutomated bump by `pulp pr`. Surfaces moved:\n";
    std::istringstream lines(staged.stdout_text);
    std::string line;
    while (std::getline(lines, line)) {
        auto t = trim(line);
        if (t.empty()) continue;
        msg << "  - " << t << "\n";
    }

    std::string cmd = "git -C '" + root.string() + "' -c commit.gpgsign=false commit -m \""
                    + msg.str() + "\"";
    // Escape any embedded newlines for the shell — use a here-doc via env var.
    // Simpler: write to a temp file.
    fs::path tmp = fs::temp_directory_path() / "pulp-pr-commit-msg.txt";
    {
        std::ofstream f(tmp);
        f << msg.str();
    }
    cmd = "git -C '" + root.string() + "' -c commit.gpgsign=false commit -F '" + tmp.string() + "'";
    int rc = run_passthrough(cmd);
    fs::remove(tmp);
    return rc;
}

int step_gh_pr_create(const fs::path& root, const std::string& title, const std::string& body) {
    std::cout << color::bold() << "▸ Creating PR via gh" << color::reset() << "\n";
    fs::path tmp = fs::temp_directory_path() / "pulp-pr-body.md";
    {
        std::ofstream f(tmp);
        f << body;
    }
    std::string cmd = "cd '" + root.string() + "' && gh pr create --title '" + title
                    + "' --body-file '" + tmp.string() + "'";
    int rc = run_passthrough(cmd);
    fs::remove(tmp);
    return rc;
}

int step_shipyard_ship(const fs::path& root) {
    std::cout << color::bold() << "▸ Invoking shipyard ship" << color::reset() << "\n";
    return run_passthrough("cd '" + root.string() + "' && shipyard ship");
}

// ── Body/title generation ───────────────────────────────────────────────

std::string default_pr_title(const fs::path& root) {
    // Use the tip commit's subject by default.
    auto r = run_capture("git -C '" + root.string() + "' log -1 --format=%s HEAD");
    auto s = trim(r.stdout_text);
    if (s.empty()) s = "Update";
    return s;
}

std::string render_body(const std::string& bump_output, const std::string& skill_output) {
    std::ostringstream b;
    b << "## Automated by `pulp pr`\n\n"
      << "### Skill-sync verdict\n```\n" << skill_output << "\n```\n\n"
      << "### Version-bump verdict\n```\n" << bump_output << "\n```\n";
    return b.str();
}

// ── Flag parsing ────────────────────────────────────────────────────────

struct Options {
    std::string base = "origin/main";
    std::string title;
    bool dry_run = false;
    bool no_ship = false;       // create PR but don't invoke shipyard
    bool no_push = false;       // stop before pushing branch
};

struct WorkflowArgs {
    std::vector<std::string> stripped;
    std::string cli_override;
    std::string error;
};

void print_usage();

WorkflowArgs consume_workflow_args(const std::vector<std::string>& args) {
    WorkflowArgs out;
    out.stripped.reserve(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--workflow") {
            if (i + 1 >= args.size()) {
                out.error = "pulp pr: --workflow requires a value";
                return out;
            }
            out.cli_override = args[++i];
            continue;
        }
        const std::string prefix = "--workflow=";
        if (a.rfind(prefix, 0) == 0) {
            out.cli_override = a.substr(prefix.size());
            continue;
        }
        out.stripped.push_back(a);
    }
    return out;
}

enum class ParseStatus { ok, help, error };

ParseStatus parse_native_options(const std::vector<std::string>& args, Options& opt) {
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--help" || a == "-h") { print_usage(); return ParseStatus::help; }
        else if (a == "--dry-run") opt.dry_run = true;
        else if (a == "--no-ship") opt.no_ship = true;
        else if (a == "--no-push") opt.no_push = true;
        else if (a == "--base"  && i + 1 < args.size()) opt.base  = args[++i];
        else if (a == "--title" && i + 1 < args.size()) opt.title = args[++i];
        else { std::cerr << "pulp pr: unknown option '" << a << "'\n"; print_usage(); return ParseStatus::error; }
    }
    return ParseStatus::ok;
}

void print_usage() {
    std::cout <<
        "Usage: pulp pr [options]\n"
        "\n"
        "One-shot PR orchestrator. Default workflow delegates to `shipyard pr`.\n"
        "Explicit github/manual workflows are opt-in local bypasses.\n"
        "\n"
        "Options:\n"
        "  --base <ref>    Diff base (default: origin/main)\n"
        "  --title <s>     PR title (default: tip commit subject)\n"
        "  --workflow <m>  Override PR workflow: shipyard | github | manual\n"
        "  --no-ship       Create the PR but skip `shipyard ship`\n"
        "  --no-push       Stop after the bump commit; don't push or PR\n"
        "  --dry-run       Print the plan without running steps\n"
        "  -h, --help      Show this help\n";
}

}  // namespace

// ── Entry point ─────────────────────────────────────────────────────────

namespace {

std::string locate_tool_on_path(const std::string& name) {
    const char* path = std::getenv("PATH");
    if (!path) return {};
#if defined(_WIN32)
    const char sep = ';';
    std::vector<std::string> names{name};
    if (name.find('.') == std::string::npos) names.push_back(name + ".exe");
#else
    const char sep = ':';
    std::vector<std::string> names{name};
#endif
    std::string pathstr(path);
    std::string::size_type start = 0;
    while (start <= pathstr.size()) {
        auto end = pathstr.find(sep, start);
        auto dir = pathstr.substr(start, end - start);
        if (!dir.empty()) {
            for (const auto& exe : names) {
                fs::path candidate = fs::path(dir) / exe;
                std::error_code ec;
                if (fs::exists(candidate, ec) && !ec) {
                    return candidate.string();
                }
            }
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return {};
}

void print_install_shipyard_hint() {
    std::cerr <<
        "pulp pr: shipyard is not on PATH, and the ship flow is the one source\n"
        "of truth across pulp + shipyard.\n\n"
        "Install shipyard in a Pulp checkout:\n"
        "  ./tools/install-shipyard.sh           # downloads the pinned binary\n"
        "  export PATH=\"$HOME/.local/bin:$PATH\"  # add to your shell rc once\n\n"
        "Re-run `pulp pr` after install. If you prefer not to use Shipyard,\n"
        "choose an explicit workflow instead:\n"
        "  pulp config set pr.workflow github   # direct GitHub CLI (`gh`) PR flow\n"
        "  pulp config set pr.workflow manual   # print manual PR steps only\n"
        "  PULP_PR_WORKFLOW=github pulp pr      # one-off override\n\n"
        "If you're debugging the native pulp-cli implementation of the same\n"
        "flow, re-run with:\n"
        "  pulp pr --native\n";
}

void print_install_gh_hint() {
    std::cerr <<
        "pulp pr: PR workflow is `github`, but the GitHub CLI (`gh`) is not on PATH.\n\n"
        "`github` is Pulp's direct GitHub workflow name; `gh` is the command-line\n"
        "tool it uses to authenticate, push metadata, and create the PR.\n\n"
        "Install and authenticate `gh`, then retry:\n"
        "  brew install gh        # macOS/Homebrew example\n"
        "  gh auth login\n\n"
        "Or switch workflows:\n"
        "  pulp config set pr.workflow shipyard\n"
        "  pulp config set pr.workflow manual\n";
}

int print_manual_workflow_plan(const std::vector<std::string>& args) {
    Options opt;
    auto parsed = parse_native_options(args, opt);
    if (parsed == ParseStatus::help) return 0;
    if (parsed == ParseStatus::error) return 2;

    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "pulp pr: not inside a pulp source checkout (no CMakeLists.txt + core/ found)\n";
        return 2;
    }

    auto branch = current_branch(root);
    if (branch.empty()) branch = "<feature-branch>";
    auto title = opt.title.empty() ? default_pr_title(root) : opt.title;

    std::cout
        << "pulp pr: manual PR workflow selected; no commands were run.\n\n"
        << "Suggested manual sequence for this checkout:\n"
        << "  cd " << shell_quote(root) << "\n"
        << "  python3 tools/scripts/skill_sync_check.py --base " << shell_quote(opt.base)
        << " --mode=report\n"
        << "  python3 tools/scripts/version_bump_check.py --base " << shell_quote(opt.base)
        << " --mode=apply\n"
        << "  git status --short\n"
        << "  git push -u origin " << shell_quote(branch) << "\n"
        << "  gh pr create --title " << shell_quote(title) << "\n\n"
        << "Manual and GitHub workflows do not create Shipyard tracking state.\n"
        << "Switch back with: pulp config set pr.workflow shipyard\n";
    return 0;
}

int exec_shipyard_pr(const std::string& shipyard_bin,
                     const std::vector<std::string>& args) {
    std::ostringstream cmd;
    cmd << shell_quote(shipyard_bin) << " pr";
    for (const auto& a : args) {
        cmd << " " << shell_quote(a);
    }
    // run_passthrough streams stdio through the shell so the user sees
    // shipyard's colored output live.
    return run_passthrough(cmd.str());
}

// Exact-equality guard. Returns 0 (pass) or 2 (fail + printed error).
// Skipped entirely when PULP_PR_SKIP_VERSION_GUARD=1 is set.
int enforce_shipyard_version_pin(const fs::path& root,
                                 const std::string& shipyard_bin) {
    if (const char* skip = std::getenv("PULP_PR_SKIP_VERSION_GUARD");
        skip && std::string(skip) == "1") {
        std::cerr << color::yellow()
                  << "pulp pr: PULP_PR_SKIP_VERSION_GUARD=1 — bypassing "
                     "shipyard version pin check.\n"
                  << color::reset();
        return 0;
    }
    auto pinned = read_pinned_shipyard_version(root);
    if (pinned.empty()) return 0;  // can't verify → proceed
    auto actual = capture_shipyard_version(shipyard_bin);
    if (actual.empty()) return 0;  // can't verify → proceed
    if (actual == pinned) return 0;

    std::cerr << color::red() << "pulp pr: shipyard version pin mismatch.\n"
              << color::reset()
              << "\n"
              << "  pinned in tools/shipyard.toml : " << pinned << "\n"
              << "  shipyard --version            : " << actual << "\n"
              << "  resolved from                 : " << shipyard_bin << "\n"
              << "\n"
              << "Fix one of:\n"
              << "  (a) Reinstall the pinned binary to $HOME/.local/bin and\n"
              << "      guarantee it's first on PATH:\n"
              << "          ./tools/install-shipyard.sh\n"
              << "  (b) Remove the stale binary at the path above:\n"
              << "          rm " << shipyard_bin << "\n"
              << "      (If it was installed via `uv tool install shipyard`,\n"
              << "      also run `uv tool uninstall shipyard`.)\n"
              << "\n"
              << "Run `shipyard doctor` for the full picture.\n"
              << "Bypass for intentional off-pin testing:\n"
              << "  PULP_PR_SKIP_VERSION_GUARD=1 pulp pr\n";
    return 2;
}

}  // namespace

int cmd_pr(const std::vector<std::string>& args) {
    // Filter out `--native` before any other option lookup so the shim
    // decision is independent of the native parser's flags.
    bool force_native = false;
    std::vector<std::string> without_native;
    without_native.reserve(args.size());
    for (const auto& a : args) {
        if (a == "--native") { force_native = true; continue; }
        without_native.push_back(a);
    }

    auto workflow_args = consume_workflow_args(without_native);
    if (!workflow_args.error.empty()) {
        std::cerr << workflow_args.error << "\n";
        return 2;
    }

    auto workflow = resolve_pr_workflow(workflow_args.cli_override);
    if (!workflow.error.empty()) {
        std::cerr << "pulp pr: invalid PR workflow '" << workflow.workflow
                  << "' from " << workflow.source << "\n"
                  << "         " << workflow.error << "\n";
        return 2;
    }

    auto forward = workflow_args.stripped;
    if (!force_native) {
        if (!forward.empty() && (forward[0] == "--help" || forward[0] == "-h")) {
            // `pulp pr --help` always shows the shim's help too, so the
            // user learns about --native. Then fall through to shipyard's
            // own help when the selected workflow is Shipyard.
            std::cout <<
                "Usage: pulp pr [--native] [--workflow shipyard|github|manual] [options]\n"
                "\n"
                "Default workflow: shipyard. Override once with --workflow or\n"
                "PULP_PR_WORKFLOW, or persist with `pulp config set pr.workflow ...`.\n"
                "Pass --native to run the in-CLI fallback implementation instead\n"
                "(for diagnostics).\n\n";
        }

        if (workflow.workflow == "shipyard") {
            auto shipyard = locate_tool_on_path("shipyard");
            if (!shipyard.empty()) {
                if (auto root = find_project_root(); !root.empty()) {
                    if (int rc = enforce_shipyard_version_pin(root, shipyard); rc != 0) {
                        return rc;
                    }
                }
                return exec_shipyard_pr(shipyard, forward);
            }
            print_install_shipyard_hint();
            return 2;
        }

        if (workflow.workflow == "manual") {
            return print_manual_workflow_plan(forward);
        }
    }

    const bool github_workflow = !force_native && workflow.workflow == "github";

    // ── Native fallback / explicit GitHub workflow ──────────────────────
    Options opt;
    auto parsed = parse_native_options(forward, opt);
    if (parsed == ParseStatus::help) return 0;
    if (parsed == ParseStatus::error) return 2;

    if (github_workflow) {
        opt.no_ship = true;
        if (!opt.dry_run && !opt.no_push && locate_tool_on_path("gh").empty()) {
            print_install_gh_hint();
            return 2;
        }
        std::cerr << color::yellow()
                  << "pulp pr: using github workflow via `gh`; Shipyard tracking "
                     "and merge validation are disabled.\n"
                  << color::reset();
    }

    if (force_native && !opt.dry_run && !opt.no_push && !opt.no_ship &&
        locate_tool_on_path("shipyard").empty()) {
        print_install_shipyard_hint();
        return 2;
    }

    auto root = find_project_root();
    if (root.empty()) {
        std::cerr << "pulp pr: not inside a pulp project (no CMakeLists.txt found)\n";
        return 2;
    }

    auto branch = current_branch(root);
    if (branch.empty() || branch == "main") {
        std::cerr << "pulp pr: refusing to run on '" << branch
                  << "' — create a feature branch first.\n";
        return 2;
    }

    if (opt.dry_run) {
        std::cout << "[dry-run] Plan:\n"
                  << "  1. skill_sync_check --base " << opt.base << "\n"
                  << "  2. version_bump_check --mode=apply\n"
                  << "  3. git commit if files staged\n"
                  << "  4. git push -u origin " << branch << "\n"
                  << "  5. gh pr create\n";
        if (!opt.no_ship) std::cout << "  6. shipyard ship\n";
        return 0;
    }

    // Best-effort heads-up: warn if RELEASE_BOT_TOKEN is missing on the
    // active GitHub repo. Doesn't block — gates and merge are unaffected
    // — but surfacing the trap here means the user finds out before they
    // wonder why the post-merge GitHub Release never appeared.
    {
        auto repo = ::trim(run_capture("gh repo view --json nameWithOwner -q .nameWithOwner 2>/dev/null").stdout_text);
        if (!repo.empty() && repo.find('/') != std::string::npos) {
            // Probe without --jq so we can distinguish "gh errored" (empty
            // stdout) from "repo has zero secrets" (non-empty JSON, no
            // matches). Codex P1 on #149: the previous guard gated on
            // `!secrets.empty()` which collapsed both cases into silence
            // and suppressed the warning in the exact bootstrap scenario
            // where the user needs it most. --paginate also handles repos
            // with many secrets where the token might be on page 2+.
            auto raw = run_capture(
                "gh api 'repos/" + repo + "/actions/secrets' --paginate 2>/dev/null"
            ).stdout_text;
            bool gh_call_succeeded = !raw.empty();
            bool present = raw.find("\"name\":\"RELEASE_BOT_TOKEN\"") != std::string::npos
                        || raw.find("\"name\": \"RELEASE_BOT_TOKEN\"") != std::string::npos;
            if (gh_call_succeeded && !present) {
                std::cerr << color::yellow()
                          << "\n▸ Heads-up: RELEASE_BOT_TOKEN secret is missing on "
                          << repo << ".\n"
                          << "         Auto-release will tag the version bump but "
                             "the binary release workflows won't fire.\n"
                          << "         Run `pulp doctor` for the one-time setup steps "
                             "(or see docs/guides/versioning.md).\n"
                          << color::reset();
            }
        }
    }

    // Step 1: skill-sync — must pass before any file is rewritten.
    if (int rc = step_skill_sync(root, opt.base); rc != 0) {
        std::cerr << "\npulp pr: skill-sync gate failed. Update the listed SKILL.md(s)\n"
                     "         or add a `Skill-Update: skip skill=<name> reason=\"...\"`\n"
                     "         trailer on the tip commit, then retry.\n";
        return rc;
    }

    // Capture output for the PR body.
    auto bump_out = run_capture("python3 '" + (root / "tools/scripts/version_bump_check.py").string()
                                 + "' --base '" + opt.base + "' --mode=report");
    auto skill_out = run_capture("python3 '" + (root / "tools/scripts/skill_sync_check.py").string()
                                 + "' --base '" + opt.base + "' --mode=report");

    // Step 2: apply version bumps in place.
    if (int rc = step_version_bump(root, opt.base); rc != 0) {
        std::cerr << "\npulp pr: version-bump apply failed. See output above.\n";
        return rc;
    }

    // Step 3: commit the staged bump(s), if any.
    if (int rc = step_commit_bumps(root); rc != 0) {
        std::cerr << "\npulp pr: commit step failed.\n";
        return rc;
    }

    if (opt.no_push) {
        std::cout << "\npulp pr: --no-push set; stopping before push.\n";
        return 0;
    }

    // Step 4: push upstream.
    if (!push_upstream(root, branch)) {
        std::cerr << "\npulp pr: git push failed.\n";
        return 1;
    }

    if (!has_clean_worktree(root)) {
        std::cerr << "\npulp pr: worktree not clean after push — refusing to create PR.\n";
        return 1;
    }

    // Step 5: create the PR.
    std::string title = opt.title.empty() ? default_pr_title(root) : opt.title;
    std::string body  = render_body(bump_out.stdout_text, skill_out.stdout_text);
    if (int rc = step_gh_pr_create(root, title, body); rc != 0) {
        std::cerr << "\npulp pr: gh pr create failed.\n";
        return rc;
    }

    if (opt.no_ship) {
        std::cout << "\npulp pr: --no-ship set; PR is open but not yet merged.\n";
        if (github_workflow) {
            std::cout << "pulp pr: github workflow leaves Shipyard tracking disabled.\n";
        }
        return 0;
    }

    // Step 6: hand off to shipyard for validation + merge.
    if (int rc = step_shipyard_ship(root); rc != 0) {
        std::cerr << "\npulp pr: shipyard ship failed.\n";
        return rc;
    }

    std::cout << "\npulp pr: done.\n";
    return 0;
}
