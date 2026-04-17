// cmd_pr.cpp — `pulp pr` subcommand.
//
// Primary behavior: delegate to `shipyard pr` when shipyard is installed.
// Shipyard is Pulp's single-source-of-truth ship orchestrator; keeping a
// parallel native implementation in two tools is how drift starts. See
// issue #352.
//
// Fallback: if shipyard is not on PATH, print a concise install guide
// (tools/install-shipyard.sh + PATH hint) and exit 2. `--native` forces
// the in-CLI implementation below, which stays as a diagnostic fallback
// when shipyard itself is broken or under debug.
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

void print_usage() {
    std::cout <<
        "Usage: pulp pr [options]\n"
        "\n"
        "One-shot PR orchestrator: skill-sync check -> version-bump apply ->\n"
        "commit -> push -> gh pr create -> shipyard ship.\n"
        "\n"
        "Options:\n"
        "  --base <ref>    Diff base (default: origin/main)\n"
        "  --title <s>     PR title (default: tip commit subject)\n"
        "  --no-ship       Create the PR but skip `shipyard ship`\n"
        "  --no-push       Stop after the bump commit; don't push or PR\n"
        "  --dry-run       Print the plan without running steps\n"
        "  -h, --help      Show this help\n";
}

}  // namespace

// ── Entry point ─────────────────────────────────────────────────────────

namespace {

// Resolve `shipyard` on PATH without depending on platform shell helpers.
// Honours PATH env; matches how `find_on_path` works in platform/, but
// local so cmd_pr.cpp keeps no new link-level dependencies.
std::string locate_shipyard() {
    const char* path = std::getenv("PATH");
    if (!path) return {};
#if defined(_WIN32)
    const char sep = ';';
    const char* exe = "shipyard.exe";
#else
    const char sep = ':';
    const char* exe = "shipyard";
#endif
    std::string pathstr(path);
    std::string::size_type start = 0;
    while (start <= pathstr.size()) {
        auto end = pathstr.find(sep, start);
        auto dir = pathstr.substr(start, end - start);
        if (!dir.empty()) {
            fs::path candidate = fs::path(dir) / exe;
            std::error_code ec;
            if (fs::exists(candidate, ec) && !ec) {
                return candidate.string();
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
        "  export PATH=\"$HOME/.pulp/bin:$PATH\"   # add to your shell rc once\n\n"
        "Re-run `pulp pr` after install. If you're debugging the native\n"
        "pulp-cli implementation of the same flow, re-run with:\n"
        "  pulp pr --native\n";
}

int exec_shipyard_pr(const std::string& shipyard_bin,
                     const std::vector<std::string>& args) {
    std::ostringstream cmd;
    // Quote the binary path in case it sits under a space-containing path.
    cmd << '\'' << shipyard_bin << "' pr";
    for (const auto& a : args) {
        cmd << " '" << a << '\'';
    }
    // run_passthrough streams stdio through the shell so the user sees
    // shipyard's colored output live.
    return run_passthrough(cmd.str());
}

}  // namespace

int cmd_pr(const std::vector<std::string>& args) {
    // Filter out `--native` before any other option lookup so the shim
    // decision is independent of the native parser's flags.
    bool force_native = false;
    std::vector<std::string> forward;
    forward.reserve(args.size());
    for (const auto& a : args) {
        if (a == "--native") { force_native = true; continue; }
        forward.push_back(a);
    }

    if (!force_native) {
        if (!forward.empty() && (forward[0] == "--help" || forward[0] == "-h")) {
            // `pulp pr --help` always shows the shim's help too, so the
            // user learns about --native. Then fall through to shipyard's
            // own help when available.
            std::cout <<
                "Usage: pulp pr [--native] [<shipyard pr options>]\n"
                "\n"
                "By default this delegates to `shipyard pr`, the canonical\n"
                "push-a-PR orchestrator. Pass --native to run the in-CLI\n"
                "fallback implementation instead (for diagnostics).\n\n";
        }
        auto shipyard = locate_shipyard();
        if (!shipyard.empty()) {
            return exec_shipyard_pr(shipyard, forward);
        }
        print_install_shipyard_hint();
        return 2;
    }

    // ── Native fallback (pre-shipyard behaviour) ────────────────────────
    Options opt;
    for (size_t i = 0; i < forward.size(); ++i) {
        const auto& a = forward[i];
        if (a == "--help" || a == "-h") { print_usage(); return 0; }
        else if (a == "--dry-run") opt.dry_run = true;
        else if (a == "--no-ship") opt.no_ship = true;
        else if (a == "--no-push") opt.no_push = true;
        else if (a == "--base"  && i + 1 < forward.size()) opt.base  = forward[++i];
        else if (a == "--title" && i + 1 < forward.size()) opt.title = forward[++i];
        else { std::cerr << "pulp pr: unknown option '" << a << "'\n"; print_usage(); return 2; }
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
