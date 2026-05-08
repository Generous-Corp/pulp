---
name: ci
description: Local and cloud CI for Pulp — validate branches, create PRs, merge on green. Handles "push to main", "ship this", "run CI", "check PR", and "list PRs".
requires:
  scripts:
    - tools/local-ci/local_ci.py
  tools:
    - gh
---

# CI Skill

Validate branches and ship code safely. This skill handles all CI workflows for Pulp across local machines and VMs.

## Pre-flight: plugin ↔ CLI skew check

Before shelling out to `pulp` (or `shipyard pr`, which ultimately
invokes `pulp`), source the shared skew-check helper so a user on an
outdated CLI sees a one-line hint rather than running into obscure
flag-missing errors mid-ship:

```bash
source "$(git rev-parse --show-toplevel)/tools/scripts/cli_version_check.sh"
pulp_cli_version_check
```

This is advisory only — the helper never blocks. See the `upgrade`
skill for the full banner contract and override knobs
(`PULP_SKEW_CHECK_DISABLE`, `PULP_SKEW_CHECK_CACHE`). Release-discovery
Slice 6 (#551).

## Shipping a PR: route through `shipyard pr`

When the user says any of: **"push to main"**, **"ship this"**, **"ship it"**,
**"we're done"**, **"merge this"**, **"push it"**, **"run CI"**, **"push a PR"** —
run `shipyard pr` (not `gh pr create` + `shipyard ship` separately).

`shipyard pr` is the single orchestrator (Shipyard v0.19.1+; currently pinned
in `tools/shipyard.toml`). It:

1. Calls `tools/scripts/skill_sync_check.py` (resolved via Shipyard's
   `[validation]` path-discovery, explicit in `.shipyard/config.toml`) and
   hard-fails if a mapped skill path was touched without a `SKILL.md` update
   or a `Skill-Update:` trailer.
2. Calls `tools/scripts/version_bump_check.py --mode=apply` to bump SDK,
   Claude plugin, and marketplace versions consistently, honoring any
   `Version-Bump:` trailers.
3. Commits the bump (if any) as `chore: bump <surfaces>`.
4. Pushes the branch, creates the PR, and records Shipyard tracking state.
5. Runs cross-platform validate + merge on green.
6. Auto-release workflow (`.github/workflows/auto-release.yml`) tags and
   publishes binaries on merge.

Never run `gh pr create` + `shipyard ship` separately for a normal ship
cycle. Never invoke the two version/skill scripts by hand — `shipyard pr`
wires them together with the right flags.

Direct `gh pr create` is an explicit emergency/manual bypass only. If the
user asks for that path, state the tracking gap up front: the PR may not
appear in Shipyard-managed state or the macOS GUI until it is reconciled or
re-shipped through Shipyard.

`pulp pr` is a Pulp-side wrapper that delegates to `shipyard pr`; both are
valid, agents should prefer `shipyard pr` for directness. Humans can opt out
of Shipyard for their own checkout with `pulp config set pr.workflow github`
or `manual`, but agents should not choose those workflows unless the user
explicitly asks for a manual/emergency bypass. `pulp status` reports the
effective workflow and whether its required local tool is installed.

Backward compatibility: raw `shipyard ship` / `shipyard run` still work for
diagnostics, experimental branches, existing Shipyard-managed PRs, or when
`shipyard pr` itself is being debugged. Do not use them as the primary ship
path.

### Shipyard pin and behaviour notes

Pin bumps must go through `shipyard pin bump --to vX.Y.Z`, not a hand edit.
Shipyard v0.50.0+ is Rust-backed and macOS ships as an Apple-Silicon-only
signed/notarized `.dmg`, so the version and asset metadata must move together.

- **Release SDKs are expected to include desktop WebView symbols**
  (pulp #695). `.github/workflows/release-cli.yml` now configures the
  release build with `-DPULP_BUILD_WEBVIEW=ON`, installs Linux's
  `libgtk-3-dev` + `libwebkit2gtk-4.1-dev`, and verifies the staged
  `pulp-view` archive still contains `WebViewPanel` and
  `make_webview_embedded_resource_fetcher`. If you touch the release
  workflow or `tools/scripts/release-cli-local.sh`, preserve that
  contract or WebView-using downstream SDK consumers will link-fail.
- **Phase 8 CLI flip ships two CLI binaries.** Release CLI jobs must
  preserve Rust `pulp` as the user-facing binary and C++ `pulp-cpp`
  as the fallthrough delegate in the same archive. Smoke both names:
  `pulp version --json` for the Rust path, and at least one C++-owned
  command through `PULP_RS_CPP_BINARY=/path/to/pulp-cpp pulp ...` or a
  direct `pulp-cpp ...` invocation. Do not resurrect `pulp-rs` as the
  shipped binary name.
- **macOS binary is signed + notarized** (Shipyard v0.29.0). On
  macOS 26.3+ XProtect skips the deep scan for notarized binaries,
  cutting `shipyard pr` cold-start ~4-5x (from ~5-6s to ~1-1.5s).
  No pulp-side action; transparent.
- **Heartbeat line during long validation** (Shipyard v0.29.0). A
  20-minute lane now prints periodic progress instead of leaving a
  silent terminal. Helpful when watching `shipyard ship` interactively.
- **Backend errors are surfaced under the summary table** (Shipyard
  v0.28.0). A bare `ubuntu     error     ssh    12s` row used to
  give zero diagnostic signal — the captured stderr tail (bundle
  upload failure, remote `cmake` apply failure, SSH transport
  error, etc.) now prints below the table. Closes pulp #665's
  diagnosis-blind-spot complaint.
- **Worktree-local `.shipyard.local/` falls back to the primary
  checkout** (Shipyard v0.27.2). Pulp uses worktrees heavily for
  parallel agent work; without this every new worktree had to
  manually `cp .shipyard.local/config.toml` from the primary repo
  before `shipyard pr` could see the SSH host config. Now it
  inherits automatically.
- **Ship preflight runs BEFORE `gh pr create`** (Shipyard v0.27.1).
  Earlier the PR was opened first and ship aborted on unreachable
  SSH backend, leaving stranded PRs with no validation (the
  Apr 22 pattern that left several pulp PRs mid-flight). Now an
  unreachable target fails fast and the PR is never opened.
- **Daemon tunnel supervisor** (Shipyard v0.27.0). Tailscale Funnel
  transients no longer kill the daemon — the supervisor restarts
  the funnel on backoff. Periodic reconcile loop runs independently
  of per-PR polls. Both apply to the macOS GUI's webhook delivery.
- **Long-running daemons keep accepting fresh subscribers** (Shipyard
  v0.26.0). The subscribe-replay path now uses blocking `put()`
  instead of `put_nowait()`, so once the replay ring grows past 64
  events the daemon no longer silently stops handing new IPC clients
  their initial snapshot. This was the root cause of the macOS GUI
  falling back to "polling" mode and showing no active PRs after
  enough ship-state churn.
- **Daemon/CLI drift is now diagnosable over IPC** (Shipyard v0.26.0).
  IPC protocol is bumped to 2, daemon hello/status frames advertise
  the daemon's own `shipyard_version`, and `shipyard doctor` flags a
  daemon-vs-CLI version mismatch explicitly instead of leaving stale
  subscribers to fail mysteriously.
- **Auto-PR titles and bodies use the feature commit** (Shipyard v0.24.0 /
  Shipyard #151). The orchestrator walks past the mechanical
  `chore: bump versions` commit when composing the title/body, and scrubs
  the `Automated by shipyard pr.` tool-branding text. Pulp PR #624 was the
  canonical repro before the fix. Previously the auto-PR pointed at the
  bump commit — generic and uninformative. Shipped PRs now read as
  first-party.
- **`Version-Bump:` trailers are authoritative, not ceiling-raising**
  (Shipyard v0.25.0 / Shipyard #152). An author-declared
  `Version-Bump: <surface>=patch reason="..."` is no longer silently
  raised to `minor` when the conventional-commit heuristic on other
  subjects classifies the diff as `minor`. This matches the pulp-side
  behaviour in `tools/scripts/version_bump_check.py` at this pin.
- **`shipyard ship-state list` is served from the daemon via IPC** when
  `shipyard daemon` is running (Shipyard v0.25.0 / Shipyard #154). The
  PyInstaller cold-start (~5-6s) is bypassed. Callers that tight-loop
  over ship-state — the macOS GUI polls every 7s; `pulp pr` preflight
  calls it indirectly — see a meaningful CPU saving. Nothing to do at
  the pulp side; it's transparent.

### SSH preflight (v0.20.0+ / Shipyard #106)

Exit codes are distinct:

| Exit | Meaning |
|------|---------|
| 0 | Success |
| 1 | Validation failed |
| 2 | Configuration error |
| 3 | Backend unreachable (new; surfaces within 10s with classified reason) |

The unreachable-backend error names the failure class (auth / host_key /
network / timeout / configuration / unknown) and prints the last ssh stderr.

Flags:

- `--skip-target NAME` — **DELIBERATE** lane skip (no probe runs). Use when
  you know a target is irrelevant for this PR.
- `--allow-unreachable-targets` — proceed despite an unreachable backend.
  Prints a loud `⚠︎ VALIDATION GAP: <target> skipped` banner. Use only when
  you genuinely cannot reach a backend and accept the validation gap.

Automation (crons, agents) should branch on exit code 3 specifically rather
than parsing error strings.

## Tool selection: Shipyard (primary)

**Shipyard is Pulp's primary CI tool.** All merges, validations, and
ship cycles should use Shipyard. `local_ci.py` remains in the repo as
a fallback but is scheduled for removal after a 2-week observation
period (see danielraffel/pulp#120).

```bash
# Primary: Shipyard
shipyard run                              # validate current branch
shipyard pr                               # create, track, validate, and merge on green
shipyard ship --resume                    # pick up an interrupted ship (v0.3.0+)
shipyard ship --no-resume                 # discard stale state, start fresh
shipyard ship-state list                  # in-flight ships (title, url, sha)
shipyard ship-state show <pr>             # full state for one PR
shipyard ship-state discard <pr>          # archive stale state
shipyard cleanup --ship-state --apply     # prune closed-PR + aged state
shipyard run --targets windows --smoke    # fast Windows-only check
shipyard run --resume-from test           # skip configure+build, run tests only
shipyard cloud run build <branch>         # dispatch to Namespace

# Target management
shipyard targets                          # list targets with reachability
shipyard targets test windows             # probe a single target

# Config inspection
shipyard config show                      # effective merged config
shipyard config profiles                  # list profiles + active

# Fallback only (if Shipyard is broken or unavailable):
python3 tools/local-ci/local_ci.py run
python3 tools/local-ci/local_ci.py ship
```

### Resuming an interrupted ship (v0.3.0+)

If a ship was interrupted (laptop closed, session ended, OS restart),
run `shipyard ship` again — it auto-resumes from a per-PR state file
at `<state_dir>/ship/<pr>.json` without re-dispatching. Shipyard
refuses to resume if the PR head SHA or merge policy changed since
the state was written; re-run with `--no-resume` to discard and ship
fresh.

`shipyard ship-state list` is the self-describing inventory (PR,
title, URL, tip commit subject, dispatched-run IDs). Come back to
a week-old laptop state and still know what you were shipping.

### Fast test iteration on SSH targets

`--resume-from` now works on SSH and SSH-Windows targets. Shipyard
probes the remote for a marker file proving the previous stage
passed for the exact SHA. If found, earlier stages are skipped:

```bash
# After a full build passes, iterate on test failures only:
shipyard run --targets windows --resume-from test   # ~2 min vs 15 min

# Resume from build (skip setup + configure):
shipyard run --resume-from build
```

### Iterating on a single-platform failure

When CI goes red on exactly one of Pulp's platforms — e.g. only the
Windows Coverage leg of the #566 matrix, only the macOS AddressSanitizer
leg, only Linux Namespace — **do not default to pushing a fix and
waiting for the full matrix**. That re-validates `mac`, `ubuntu`, and
`windows` on every iteration when only one of them actually failed,
burning ~25 minutes of wall time and GitHub Actions runner minutes on
legs that were already green.

Use `shipyard run` with target selection against Pulp's real lanes:

```bash
# Iterate on the Windows lane only
shipyard run --skip-target mac --skip-target ubuntu --json

# Inclusive form (equivalent)
shipyard run --targets windows --json

# Chain with --resume-from when the build is already green and you're
# only iterating on test failures on that platform:
shipyard run --targets windows --resume-from test --json
```

This validates locally via Pulp's configured backend for the target
(`local` on mac, `ssh` on ubuntu, `ssh-windows` on windows) — see
`shipyard targets list` for the live mapping. Real result in ~5–10
min per target, zero GitHub Actions spend, no re-validation of lanes
you didn't touch. Once the local lane goes green, push + let CI
confirm across the full matrix; only then run `shipyard pr` for the
final merge gate.

**When this loop doesn't fit — keep using the full path:**

- **Final pre-merge gate.** `shipyard pr` is still the only thing that
  produces a merge-eligible evidence record across all three lanes.
  Local iteration gets you to green; `shipyard pr` lands it.
- **Failure specific to a GitHub-hosted lane.** The build matrix on
  main has a `macOS (ARM64) [github-hosted]` leg and `[namespace]`
  Linux/Windows legs. If a failure is specific to the github-hosted
  macOS environment, `shipyard run --targets mac` hits the local mac
  backend which is close but not identical. `shipyard cloud run build
  <branch>` dispatches to the same Namespace runners CI uses without
  re-running everything — the middle ground when you need the exact
  cloud environment.
- **Cross-target behavior you're actually testing.** If the bug only
  manifests when two targets interact (rare — e.g. shared FetchContent
  cache corruption), the single-target loop hides it. Full matrix
  only in that case.

**When `shipyard run` fails for reasons that don't match your change:**

Pulp's `ssh` (ubuntu) and `ssh-windows` (`win` alias) backends
accumulate per-run state — `.shipyard-stage-build-*`,
`.shipyard-stage-configure-*`, a stale worktree branch checkout from
an interrupted earlier run. If `shipyard run --targets windows` errors
with messages that look unrelated to the code you changed (CMake
complaining about files you didn't touch, configure steps timing out
on line one, paths pointing at an earlier branch), the host state is
suspect — your code change probably isn't wrong. Diagnose before
iterating:

On Linux (ssh ubuntu), the checkout is at `~/pulp-validate`; diagnose
with standard POSIX commands:

```bash
ssh ubuntu
cd ~/pulp-validate
git log -1 --oneline && git status --short       # expected SHA? clean worktree?
ls -la .shipyard-stage-* 2>/dev/null             # leftover stage dirs?
rm -rf .shipyard-stage-*                         # safe — shipyard re-stages from scratch
```

On Windows (ssh win), the checkout is at `C:\Users\danielraffel\pulp-validate`.
OpenSSH on Windows runs commands through `cmd.exe` by default, so use
cmd-native syntax — do NOT paste Windows-style backslash paths into a
`bash` block (backslashes get interpreted as escapes):

```bat
:: Via ssh from your Mac; each command is a separate ssh call so cmd.exe parses cleanly
ssh win "cd /d C:\Users\danielraffel\pulp-validate && git log -1 --oneline"
ssh win "cd /d C:\Users\danielraffel\pulp-validate && git status --short"
ssh win "cd /d C:\Users\danielraffel\pulp-validate && dir /b .shipyard-stage-*"
```

PowerShell is reliable for the removal step (the `for /d` cmd idiom is
fragile when shipped through ssh argv quoting):

```bash
ssh win 'powershell -NoProfile -Command "cd C:\Users\danielraffel\pulp-validate; Get-ChildItem -Directory -Filter .shipyard-stage-* | Remove-Item -Recurse -Force"'
```

On a genuinely stale host (validate worktree stuck on a several-weeks-old
commit with 20+ `.shipyard-stage-*` artefacts), combine `git fetch origin &&
git reset --hard origin/main` on the validate checkout with the stage
directory cleanup above. Re-run `shipyard run --targets <host>` after
cleanup.

### Incremental bundles (automatic)

SSH validation now sends only the git delta between the remote HEAD
and the target SHA. Typical cycles drop from ~443 MB to a few KB.
No configuration needed — falls back to full bundle automatically.

To install Shipyard locally for the first time:

```bash
./tools/install-shipyard.sh           # download + verify pinned binary
./tools/install-shipyard.sh --status  # show installed vs pinned version
export PATH="$HOME/.local/bin:$PATH"  # add ~/.local/bin to PATH (one-time)
```

The public Pulp installer intentionally does not install Shipyard or GitHub
CLI (`gh`). Ordinary Pulp users can create, build, run, and upgrade projects
without either tool. Treat them as source-checkout contributor dependencies
for PR/CI work; `gh` is required only for GitHub-facing maintenance commands
and the explicit `pr.workflow=github` bypass.

After install, every Pulp checkout that has `~/.local/bin` on PATH gets
the same pinned Shipyard version automatically. The pin lives in
`tools/shipyard.toml` and is bumped via PR after each Shipyard release
that passes Pulp's CI matrix. Use `shipyard pin bump --to vX.Y.Z`
instead of hand-editing `tools/shipyard.toml`; the helper owns the pin
edit and worktree-safety checks.

The two tools cover the same target matrix (mac local + GitHub-hosted
Linux/Windows; legacy SSH targets only when explicitly requested) and accept
the same `--base` flag for develop branches. Shipyard adds evidence-gated
merge that checks per-platform proof for the exact merge-candidate SHA, which
is stricter than `local_ci.py`'s `job.passed` check.

## Prerequisites Check

Before running any CI command, verify the required tooling AND provider config exists:

```bash
# Required
test -f tools/local-ci/local_ci.py || echo "ERROR: local CI not found — is this a recent checkout?"
command -v shipyard >/dev/null || echo "ERROR: shipyard not installed (run ./tools/install-shipyard.sh)"
command -v gh >/dev/null || echo "WARNING: gh CLI not installed; GitHub-facing fallback/triage commands will be unavailable"

# Preferred (shared machine-global local CI config)
test -f "$HOME/Library/Application Support/Pulp/local-ci/config.json" || echo "WARNING: no shared local CI config — copy tools/local-ci/config.example.json there"

# Fallback (worktree-local legacy config)
test -f tools/local-ci/config.json || echo "WARNING: no worktree fallback config.json"

# Verify GitHub Actions runner routing. Namespace is decommissioned.
gh variable list -R danielraffel/pulp | grep -q '^PULP_DEFAULT_RUNNER_PROVIDER[[:space:]]*github-hosted' || echo "WARNING: PULP_DEFAULT_RUNNER_PROVIDER should be github-hosted"
gh variable list -R danielraffel/pulp | grep -q '^PULP_LOCAL_MACOS_RUNS_ON_JSON' || echo "WARNING: PULP_LOCAL_MACOS_RUNS_ON_JSON is missing; macOS build will use hosted macos-15"
```

If `local_ci.py` doesn't exist, the user likely has an older checkout. Tell them to pull latest main.

## Visual Harness Container

`ci/visual-harness.Dockerfile` and `.github/workflows/visual-harness.yml`
provide the deterministic visual-harness smoke environment. The Docker image
downloads the pinned Skia `chrome/m144` Linux release asset, verifies its
SHA-256, installs the bundled Pulp fonts into fontconfig, and installs
`skia-python==144.0.post2` for the B.0 SkPicture byte-identity smoke. The
workflow runs that Linux container and also runs the same pytest smoke on
macOS arm64 so the future canonical raster lane has a platform signal. The
`macOS local smoke` job resolves `runs-on` from
`PULP_LOCAL_MACOS_RUNS_ON_JSON` first and falls back to hosted `macos-15` only
when the local selector variable is absent. On the persistent local runner,
this job deliberately uses the installed `python3.12` and a worktree-local venv
instead of `actions/setup-python`, because that action defaults to GitHub's
hosted `/Users/runner` toolcache path and can fail before tests start.

Use it when a fresh worktree has only `external/skia-build` headers/metadata
and no platform static libraries:

```bash
tools/harness/visual/docker-build.sh
docker run --rm -v "$PWD:/workspace" pulp-visual-harness
```

The wrapper defaults to the pinned Skia `linux-x64` lane (`linux/amd64`) and
keeps a reusable local buildx cache under
`~/.cache/pulp/visual-harness/buildx`. The Dockerfile also uses BuildKit cache
mounts for apt packages, the Skia release zip, and pip wheels, so repeated
runs on the same Mac/Ubuntu SSH host do not re-download the expensive inputs
unless the lock or digest changes. Override with `PULP_VISUAL_IMAGE`,
`PULP_VISUAL_DOCKER_PLATFORM`, or `PULP_VISUAL_DOCKER_CACHE` if a host needs a
separate cache namespace.

GitHub-hosted Ubuntu must create a `docker-container` Buildx builder before
calling the wrapper; the default `docker` driver on that image rejects
`type=local` cache export unless containerd image storage is enabled.

For macOS visual/layout jobs, do not use the combined `actions/cache` action
for `ccache` or FetchContent on the local self-hosted runner. Its home
directory persists between jobs, so GitHub cloud-cache saves can spend
20+ minutes uploading multi-GB compiler caches that the runner already has
locally. Use `actions/cache/restore` for GitHub-hosted fallback runners and
`actions/cache/save` only on non-PR `main` runs (`push` where the workflow has
a push trigger, or `workflow_dispatch` on `main` for manual cache seeding).
PRs should restore existing remote caches at most, not publish PR-scoped
ccache blobs.

The container is a reproducible smoke/developer environment. It does not
replace the future canonical arm64-darwin raster-golden gate.

## Language Correction

**IMPORTANT**: When a user says "push to main", "merge to main", or "land this", ALWAYS correct them:

> "I won't push directly to main — that bypasses review. Instead, I'll create a PR, run CI on it, and merge it if everything passes. This keeps main safe."

Then proceed with the `ship` workflow below.

## Runner Priority (hard rule)

**GitHub-hosted is the default runner provider** for Linux and Windows.
macOS routes to the local self-hosted runner through
`PULP_LOCAL_MACOS_RUNS_ON_JSON` when that repo variable is set; this is the
only branch-protection blocker on `main`. Namespace is decommissioned: do not
use `--mode namespace`, do not set `PULP_NAMESPACE_*_RUNS_ON_JSON`, and do not
redispatch PRs to Namespace.

The default chain (`.github/workflows/build.yml` `resolve-provider` job):

```yaml
REQUESTED_PROVIDER:
  ${{ inputs.runner_provider             # explicit workflow_dispatch input
   || vars.PULP_DEFAULT_RUNNER_PROVIDER  # repo-level override
   || 'github-hosted' }}                 # hardcoded fallback
```

Priority order:
1. **macOS local GitHub runner** — `build.yml` reads `PULP_LOCAL_MACOS_RUNS_ON_JSON` into `EXPLICIT_MACOS_RUNNER_SELECTOR_JSON`; with the usual value `["self-hosted","sanitizer"]`, the macOS build uses Daniels-MacBook-Pro.
2. **GitHub-hosted Linux/Windows** — advisory; failures should be filed as platform issues and should not block a macOS-focused merge.
3. **Legacy SSH targets** — only when the user explicitly asks. Do not use `ssh ubuntu` or `ssh win` by default.

The `resolve_runs_on.py` optional-namespace mode must still honor explicit
selectors before checking `REQUESTED_PROVIDER`. Otherwise the local macOS repo
variable is ignored when the provider is `github-hosted`, and the required
`macos` gate falls back to hosted `macos-15`.

Build and coverage checkouts keep `lfs: false` even on macOS. The repo has
LFS attributes for historical Skia binary paths, but no current CI input is a
tracked LFS object; enabling checkout LFS on the reused self-hosted workspace
causes `git lfs install --local` to fail because Pulp already owns the
`pre-push` hook.

**Self-hosted macOS build dirs must stay isolated.** The local runner keeps
`build-*` directories between workflows. The ordinary `build.yml` matrix uses
`build-${{ matrix.key }}`, and `sanitizers.yml` uses `build-asan`,
`build-tsan`, `build-ubsan`, and `build-rtsan`. Do not collapse these back to
plain `build/`: a stale sanitizer `CMakeCache.txt` can leak flags such as
`-fsanitize=address` into the required macOS build and make unrelated
JavaScriptCore/host tests abort under ASan. `tools/scripts/test_workflow_build_dirs.py`
is wired into workflow-lint to keep this invariant machine-checked. CLI
delegation to helper binaries must resolve from the active build directory
(`build-${{ matrix.key }}` or the running CLI's sibling build tree), not a
hard-coded `build/` path; otherwise Linux catches missing helpers while warm
self-hosted macOS workspaces can mask the bug.

### Overrides when you need them

- **Dispatch a specific run on github-hosted** (normal Linux/Windows path, or comparing hosted macOS behaviour):
  ```bash
  gh workflow run build.yml --repo danielraffel/pulp --ref <branch> -f runner_provider=github-hosted
  ```
- **Pin macOS to a local runner selector**: set `PULP_LOCAL_MACOS_RUNS_ON_JSON` at the repo level, or pass `macos_runner_selector_json` on a manual dispatch. Keep the selector compatible with the runner labels (`self-hosted`, `sanitizer`).
- **Do not use Namespace overrides**: any remaining Namespace variable or mode is stale configuration and should be removed rather than worked around.

## Commands

### Legacy `local_ci.py ship [branch]`

Historical fallback only. The normal workflow for "ship this" or "push to
main" is `shipyard pr`, which owns PR creation, Shipyard tracking state,
validation, and merge-on-green.

Use this only when debugging the legacy local CI controller itself. It does
not provide the same Shipyard state discipline as `shipyard pr`.

```bash
# Legacy fallback only
python3 tools/local-ci/local_ci.py ship [branch]

# Ship to a develop branch (for multi-piece features)
python3 tools/local-ci/local_ci.py ship [branch] --base develop/package-manager
```

**Develop branch workflow:** When working on complex features that use a `develop/*` integration branch, PRs target the develop branch instead of main. The develop branch itself merges to main at phase boundaries. Use `--base` to specify the target.

### `run [branch]` — Just validate, no PR

Run local CI on the current branch without creating a PR or merging.

```bash
python3 tools/local-ci/local_ci.py run [branch]
python3 tools/local-ci/local_ci.py run [branch] --smoke
```

If you pass a branch name explicitly, `run [branch]` resolves that branch tip to an exact SHA before queueing. It must not silently reuse the launching checkout's `HEAD`.

Queueing now performs a submission preflight before the job is recorded:
- prints the queued worktree root, current cwd, config path/source, and per-target host intent
- rejects accidental wrong-root launches by default
- rejects obviously unreachable SSH targets by default when they have no fallback path

Override flags exist for deliberate exceptions:

```bash
python3 tools/local-ci/local_ci.py run [branch] --allow-root-mismatch
python3 tools/local-ci/local_ci.py run [branch] --allow-unreachable-targets
```

For SSH targets, `run` uploads the exact queued SHA as a git bundle before validation, so Ubuntu and Windows do not need that branch tip to be visible on the host ahead of time.
Use `--smoke` for a fast clean install/export preflight when you want early signal before paying for the full test matrix. Smoke runs are explicitly labeled as `validation=smoke`.
If you queue a newer SHA for the same branch, targets, and validation mode, older pending work is superseded automatically.

Useful queue controls:

```bash
python3 tools/local-ci/local_ci.py run [branch] --targets mac
python3 tools/local-ci/local_ci.py run [branch] --smoke --targets windows
python3 tools/local-ci/local_ci.py enqueue [branch] --priority low
python3 tools/local-ci/local_ci.py bump <job-id> high
python3 tools/local-ci/local_ci.py logs <job-id> --target windows
python3 tools/local-ci/local_ci.py evidence [branch]
```

If the task needs GUI/session proof instead of pure build/test validation, use the desktop automation surface on the same controller:

```bash
python3 tools/local-ci/local_ci.py desktop install mac
python3 tools/local-ci/local_ci.py desktop doctor windows --json
python3 tools/local-ci/local_ci.py desktop smoke mac --bundle-id com.apple.TextEdit --label textedit-smoke
python3 tools/local-ci/local_ci.py desktop inspect mac --command '/path/to/pulp-ui-preview' --pulp-app-automation
python3 tools/local-ci/local_ci.py desktop click mac --command '/path/to/pulp-ui-preview' --click-view-id bypass-toggle --capture-ui-snapshot --pulp-app-automation
python3 tools/local-ci/local_ci.py desktop inspect windows --command 'notepad.exe' --label notepad-inspect
python3 tools/local-ci/local_ci.py desktop click windows --command 'notepad.exe' --click 885,18 --capture-before --label notepad-maximize
python3 tools/local-ci/local_ci.py desktop inspect mac --command './build-desktop-automation/examples/ui-preview/pulp-ui-preview' --source-mode exact-sha --sha <commit-sha> --prepare-command 'cmake -S . -B build-desktop-automation && cmake --build build-desktop-automation --target pulp-ui-preview' --pulp-app-automation
python3 tools/local-ci/local_ci.py desktop config set target.mac.webview_driver true
python3 tools/local-ci/local_ci.py desktop config set target.mac.webdriver_url http://127.0.0.1:4444
python3 tools/local-ci/local_ci.py desktop config set target.mac.debug_attach true
python3 tools/local-ci/local_ci.py desktop recent mac
python3 tools/local-ci/local_ci.py desktop proof windows --action inspect --source-mode exact-sha --sha <commit-sha>
python3 tools/local-ci/local_ci.py desktop publish mac --limit 5 --label mac-gallery
```

Desktop adapter truth:
- `macos-local`: local logged-in session, supports `--bundle-id` and Pulp-owned direct-app automation.
- `linux-xvfb`: `xvfb-run` GUI lane, currently `--command` + `--pulp-app-automation` for richer interaction.
- `windows-session-agent`: Scheduled Task + target-side PowerShell agent in a logged-in desktop session; supports generic `window-capture` smoke/click/inspect for normal desktop apps and reserves `--pulp-app-automation` for ViewInspector/UI-selector lanes.
- Optional WebView/debug tiers are opt-in config, not implied adapter behavior. Use `desktop status` / `desktop doctor` to confirm `optional_capabilities` before assuming `webview_dom`, `debug_attach`, `video_capture`, or `frame_stats`.

Exact-SHA desktop source guidance:
- Use `--source-mode exact-sha` when the GUI proof must match one specific commit instead of the mutable live checkout.
- Pair it with `--prepare-command` when the binary or assets must be built inside the prepared root before launch.
- Exact-SHA desktop runs persist additive provenance in `manifest.json` under `source.*` and attach `artifacts.prepare_log` when a fresh prepare step runs.
- Treat exact-SHA desktop mode as a `--command` workflow unless the adapter explicitly documents stronger support.
- Use `desktop proof` instead of ad-hoc bundle inspection when you need the latest successful GUI proof for one target/action/SHA.

### `check <PR#|URL|latest>` — Validate an existing PR

Run CI on an existing PR by number, GitHub URL, or "latest".

1. If `latest` → use `gh pr list --limit 1 --json number` to get the most recent PR
2. If URL → extract PR number from the URL
3. Fetch the PR's head branch: `gh pr view <number> --json headRefName`
4. Checkout that branch locally
5. Run local CI: `python3 tools/local-ci/local_ci.py run <branch>` or `python3 tools/local-ci/local_ci.py check <PR#> --smoke` for a fast preflight
6. Post results as a PR comment via `gh pr comment`

### `list` — Show open PRs

Show open PRs with summaries so the user can pick one to check or merge.

```bash
gh pr list --json number,title,author,headRefName,createdAt,labels --template '{{range .}}#{{.number}} {{.title}} ({{.headRefName}}) by {{.author.login}} {{timeago .createdAt}}{{"\n"}}{{end}}'
```

### `status` — Queue, live target state, and VM status

```bash
python3 tools/local-ci/local_ci.py status
```

While a job is still running, `status` can show live target state for the active job, for example `mac=pass, ubuntu=pass, windows=running`. Quiet phases should now surface heartbeat and idle/liveness hints instead of looking dead by default.

### `logs [job]` — Tail a saved target log

```bash
python3 tools/local-ci/local_ci.py logs <job-id> --target windows
```

Use this when a target looks slow or stuck. The logs come from the machine-global CI state directory, so you can inspect a running job without manual SSH.

When you need to reproduce an intermittent failure locally before spending another full CI run, use:

```bash
tools/scripts/repeat-until-fail.sh 100 -- ctest --test-dir build -R "<test name>" --output-on-failure
```

### `evidence [branch]` — Show last-good exact-SHA target evidence

```bash
python3 tools/local-ci/local_ci.py evidence
python3 tools/local-ci/local_ci.py evidence feature/my-branch --limit 3
```

Use this when a branch has been validated through narrow reruns and you need to know what is already proven without rerunning targets that already passed on the same SHA.

## Active CI Incident Loop

Do not treat a running CI job as something to check later. When a local CI job is active, one agent should own the monitoring loop and one agent or process should work the likely fix path locally as soon as a failure becomes actionable.

Required behavior while a job is active:
- Poll `python3 tools/local-ci/local_ci.py status` proactively.
- Tail `python3 tools/local-ci/local_ci.py logs <job-id> --target <name>` as soon as a target fails or looks stuck.
- Send user updates without waiting to be asked when a target changes state, a failure appears, or a rerun is queued.
- If one target has already failed, stop treating the rest of that job as the only source of truth. Start local repro or code inspection immediately when the failure is actionable.
- Once a failure is actionable, parallel work is required unless it would contend with the same host or invalidate the active run. Keep one CI owner watching the hosts and one local loop reproducing or patching the likely issue.
- Do not sit idle waiting for unrelated targets to finish if the first failing target already tells you what to investigate.
- Never rerun a target that already passed on the exact same SHA unless the prior result is untrustworthy or the environment changed.
- If only part of the matrix is stale, rerun only that subset of targets.
- Once the failure surface is isolated, prefer the minimum sufficient proof instead of a symmetric rerun. If macOS and Ubuntu already passed on the current head and only Windows changed or failed, rerun Windows only.
- A direct exact-SHA validation on one target is acceptable merge evidence for that target. Do not invalidate earlier same-SHA passes on other targets just because they came from a different run.
- Use `validation=smoke` before full CI when the risk is install/export/build structure rather than runtime test behavior.
- Treat `all targets on one SHA` as a goal, not a reason to blindly rerun already-green same-SHA targets.
- On persistent local/self-hosted targets, prefer prepared same-SHA reruns for narrow follow-up validation and make `prepared=clean` vs `prepared=reused` visible in status/logs.
- Prefer the shared machine-global CI config (`state_dir()/config.json`; on macOS `~/Library/Application Support/Pulp/local-ci/config.json`) so every worktree sees the same host map by default.
- Treat worktree-local `tools/local-ci/config.json` as a fallback or temporary override only. Hostnames and `repo_path` values can drift between worktrees.
- Pay attention to the submission preflight. If it says the cwd git root and queued worktree root differ, stop and fix that unless the mismatch is intentional.
- If preflight reports shared-state vs worktree-local config drift for the selected targets, treat that as a real warning, not cosmetic noise.
- For Windows SSH validation, prefer the configured target whose non-interactive PowerShell context resolves `git`, `cmake`, and `ctest`. Do not encode developer-specific host aliases in shared instructions; keep the selection criteria generic and environment-driven.
- If a dead runner left behind a stale Windows validator, let the queue reclaim that specific remote validator before starting fresh work; treat that cleanup as part of the truthful narrow-rerun path, not as ad hoc manual SSH.
- If a stale same-host job is still compiling an obsolete SHA or path, stop that stale process tree before spending more time diagnosing contention on the current run.

Minimum incident response once a failure is visible:
1. Capture the failing job id, target, SHA, validation mode, and first failing test/build step.
2. Decide whether the failure is infrastructure, environment drift, or a likely code/test issue.
3. If actionable, begin the fix or repro loop immediately instead of waiting for the whole matrix to finish.
4. Queue the narrowest truthful rerun needed after the fix.
5. Close the loop with a short status update that says what failed, what is being tried now, and what still remains.

### `cloud run [branch]` — Trigger GitHub Actions

Trigger cloud CI only when cloud CI is actually needed, for example
workflow-semantics changes, release validation, or a neutral-host confirmation
that local CI cannot provide. Prefer the built-in `pulp ci-local cloud ...`
surface instead of raw `gh workflow run`:

```bash
pulp ci-local cloud workflows
pulp ci-local cloud defaults
pulp ci-local cloud history
pulp ci-local cloud compare build
pulp ci-local cloud recommend build
pulp ci-local cloud run build <branch>
pulp ci-local cloud run validate <branch>
pulp ci-local cloud run docs-check <branch> --provider namespace
```

### `cloud status` — Check GitHub Actions

```bash
pulp ci-local cloud status
pulp ci-local cloud status latest --refresh
```

`cloud defaults` is the companion visibility command when you need to see the
current effective workflow/provider defaults and where Namespace selectors are
coming from before dispatching a run.

`cloud history`, `cloud compare`, and `cloud recommend` are the next visibility
layer when you need saved timing/provider evidence from earlier runs. Any cost
number shown there is `estimated; verify provider pricing`.

Use raw `gh workflow run` / `gh run view` only as a fallback when debugging the
GitHub side of the operator surface itself.

Provider truth rules:
- If a Namespace cloud dispatch fails before any matrix leg starts, inspect the GitHub run annotations and `resolve-provider` job result before blaming repo code.
- Treat provider CLI health, GitHub dispatch health, and provider billing/spend gates as separate failure surfaces.
- If cloud dispatch is blocked by billing or provider control-plane issues, cut over to the narrowest truthful local/SSH proof instead of retrying the same blocked dispatch loop.

## Configuration

Config is machine-global by default at `state_dir()/config.json` (on macOS `~/Library/Application Support/Pulp/local-ci/config.json`).
`tools/local-ci/config.json` is the gitignored fallback, and `PULP_LOCAL_CI_CONFIG` overrides both.
Template at `tools/local-ci/config.example.json`.

Key fields:
- `targets.mac.enabled` — run local Mac validation (default: true)
- `targets.ubuntu` — SSH target for Linux validation
- `targets.windows` — SSH target for Windows validation
- `targets.<name>.host` — primary SSH host alias
- `targets.<name>.fallback_host` — optional secondary SSH host alias if the primary is unreachable
- `targets.<name>.utm_fallback` — optional UTM VM to boot only if SSH hosts are unreachable
- `targets.windows.cmake_generator` / `targets.windows.cmake_platform` — optional Windows CMake generator settings; if `cmake_platform` is omitted the runner infers `ARM64` vs `x64` from the remote host
- `targets.windows.cmake_generator_instance` — optional explicit Visual Studio instance path; if omitted the runner prefers a full VS install over `BuildTools` when both exist
- `defaults.priority` — default queue priority for `run` and `enqueue`
- `defaults.ship_priority` — default queue priority for `ship`
- `defaults.check_priority` — default queue priority for `check`
- `github_actions.repository` — optional `owner/repo` override for cloud commands
- `github_actions.defaults.workflow` — default workflow key for `cloud run`
- `github_actions.defaults.provider` — default cloud runner provider
- `github_actions.defaults.wait_poll_secs` — cloud wait polling interval
- `github_actions.defaults.match_timeout_secs` — dispatch-to-run match timeout

Keep hostnames and VM names local. Shared repo docs and skills should describe how to choose a target, not which personal alias to use.

## Documentation

Full setup guide: `docs/guides/local-ci.md`
SSH key setup for Windows/Linux VMs: `docs/guides/local-ci.md` § "Set up SSH keys"

**Docs-site CI workflows:**

- `.github/workflows/docs-deploy.yml` — builds + deploys the Pulp docs
  site to GitHub Pages. As of #577 PR 4 this is **MkDocs Material only**:
  the legacy `tools/build-docs.py` generator and the
  `use_legacy_generator` workflow_dispatch fallback have both been
  deleted. Rollback, if ever needed, is via `git revert` of the
  deletion commit, not a runtime toggle. Pagefind is gone (Material
  ships its own built-in search). The workflow also invokes
  `tools/build-api-docs.sh`, which pulls the current SDK version from
  `CMakeLists.txt` and injects it into Doxygen's `PROJECT_NUMBER`.
- `.github/workflows/docs-material.yml` — parallel MkDocs Material build
  (PR-only preview) added under #577 PR 1 and extended in PR 2 to build
  Doxygen + merge `api/` into the artifact and run the
  `tools/mkdocs_hooks.py` pre-build drift checks (`docs_generate.py
  check` + `check-docs-consistency.py`) plus the URL-flatten hook. Runs
  on `pull_request` and `push` when the same docs paths change.
  **No deploy** — uploads `build/site-material/` as a 14-day artifact
  for visual review. Kept as a PR-lane preview after PR 3 so reviewers
  see rendered output before it hits production.

## Required-check ruleset (issue #462)

The branch-protection ruleset for `main` is checked into the repo at
`.github/rulesets/main-protection.json` so drift between the GitHub
ruleset UI and repo intent is visible in PR review. Pattern inspired
by [Astral's ruleset-as-code approach](https://gist.github.com/woodruffw/643a6cf70ad72d404ce6f9f333181cf8).

**Fast lane — required (blocks merge):**

- `macos` — macOS build+test leg of `.github/workflows/build.yml`
- `linux` — Linux (x64) build+test leg of `.github/workflows/build.yml`
- `windows` — Windows (x64) build+test leg of `.github/workflows/build.yml`
- `Enforce version & skill sync` — `.github/workflows/version-skill-check.yml`

The three platform names are intentionally declared as **stable aliases**
so the merge contract survives runner-provider swaps (github-hosted ↔
namespace). The concrete context strings in `build.yml` today resolve
to e.g. `macOS (ARM64) [namespace]`, which is not stable; landing the
alias layer is part of #462.

**Slow lane — advisory (does NOT block merge):**

- `AddressSanitizer (macOS ARM64)`
- `ThreadSanitizer (macOS ARM64)`
- `UndefinedBehaviorSanitizer (macOS ARM64)`
- `RealtimeSanitizer (Linux x86_64, Clang 18)`

These run via `.github/workflows/sanitizers.yml` on `workflow_dispatch`
only and are tracked in the checked-in JSON under `advisory_status_checks`
for visibility/drift, never inside `rules[].required_status_checks`.

**Drift enforcement:** `.github/workflows/ruleset-drift-check.yml` runs
on PR (when `.github/rulesets/**` changes) and weekly on cron. It fetches
the live ruleset via `gh api /repos/{owner}/{repo}/rulesets` and diffs
against the checked-in JSON. PR runs post/update a single comment; the
cron job fails loudly on drift so it shows up as a red check on `main`.

**Making a change to required checks:** always edit the JSON first, open
a PR, and let the drift-check workflow confirm the plan. Then mirror the
change in the GitHub ruleset UI (or reapply via `gh api PUT`). Never edit
the live ruleset in isolation — the next scheduled drift run will fail.

## Versioning & Skill-Sync gates (Layer 3)

`pulp pr` orchestrates the full shipping flow. CI enforces three gates on every PR to `main`:

- `.github/workflows/version-skill-check.yml` — runs `tools/scripts/version_bump_check.py`, `tools/scripts/skill_sync_check.py`, and (since #1029) `tools/scripts/compat_sync_check.py` in `--mode=report`. Failure blocks merge. No bypass except the commit trailers documented in `docs/guides/versioning.md` and `docs/guides/compat-sync.md`.
- `.shipyard/config.toml` → `[validation.gates]` pipeline — same scripts via `shipyard run --pipeline gates`. Runs with `PULP_ENFORCE_PREPUSH=1` so warnings become errors.

Locally:

- `.githooks/pre-push` (install via `tools/scripts/install-githooks.sh`) runs all three scripts advisory-by-default. `PULP_ENFORCE_PREPUSH=1` upgrades to hard fail; `PULP_SKIP_PREPUSH=1` is the single-push emergency bypass.

**Gotcha:** changing anything under `.github/workflows/**`, `tools/shipyard.toml`, `.shipyard/**`, `.githooks/**`, `tools/install-shipyard.sh`, or `tools/scripts/install-githooks.sh` triggers the skill-sync gate for the `ci` skill — keep this file in sync when those paths move. The map lives at `tools/scripts/skill_path_map.json`.

**Compat-sync (#1029):** `tools/scripts/compat_sync_check.py` is the new third leg, mirroring the skill-sync / version-bump shape for the `compat.json` matrix at the repo root. The bypass trailer is `Compat-Update: skip prefix=<section|*> reason="..."` (multiple lines allowed). Path map: `tools/scripts/compat_path_map.json`. Until #1027 ships the populated matrix, empty `compat.json` sections are tolerated. See `docs/guides/compat-sync.md` for the full design.

**Auto-release:** `.github/workflows/auto-release.yml` fires on push to `main`. It diffs the two version-bearing files (`CMakeLists.txt` project version, `.claude-plugin/plugin.json` version) against the previous push range and creates the corresponding `v<x.y.z>` or `plugin-v<x.y.z>` tag. The existing tag-triggered release workflows (`release-cli.yml`, `sign-and-release.yml`) then build and publish. `Release: skip reason="..."` on the merging commit suppresses the tag.

**fix/feat-needs-bump (issue #1009):** the version-skill-check workflow ALSO runs `version_bump_check.py --require-bump-for-fix-feat` on `pull_request` events. If the PR title matches `^(fix|feat)(\([^)]*\))?!?:\s` (Conventional Commits user-facing prefix), the diff range MUST contain either a commit subject `chore: bump versions` OR a top-level `Version-Bump: skip reason="..."` trailer (with non-empty reason — bare `skip` is rejected). This is the structural fix for the 2026-04-30 incident (PR #1008) where a `fix(view):` merged via `gh pr merge` after a force-push race with `shipyard pr` and stranded the change on main. Auto-release.yml has a matching backstop step (`Stranded fix/feat detector`) that emits a `::warning::` annotation and opens a `release-stuck`-labelled tracking issue when the merge slips through to push. Branch protection on `main` requiring the `Enforce version & skill sync` check would close the loop entirely — see `docs/guides/release-watchdog.md` for the recommended setup. Bypass the check on a one-off basis with `Version-Bump: skip reason="..."` on any commit in the range; this is intentionally a different trailer from `Release: skip` so a "don't tag this release" decision doesn't silently imply "this fix doesn't need a bump."

**Exact bump-marker format:** for `fix:` / `feat:` PR titles, the accepted
commit subject prefixes are exactly `chore: bump versions` (canonical) and
`chore(versions): bump` (legacy). A manually authored subject such as
`chore: bump SDK to v0.78.4` does not satisfy the gate, even if the
version files and changelog are correctly edited. Let `shipyard pr` create
the bump commit when possible; if you need to repair it manually, use the
canonical subject `chore: bump versions`.

**Release-workflow VST3 pin:** `sign-and-release.yml` must clone the same Steinberg tag pinned everywhere else in the repo: `v3.7.12_build_20`. The shorthand `v3.7.12` does not exist upstream and will make tag-time macOS release jobs fail before configure/build even start.

**Release-workflow ctest must skip the `validation` label (#720):** the `Test` step in `sign-and-release.yml` MUST pass `-LE validation` to ctest. Without it, the suite includes the `auval-Pulp*` tests that copy a fresh `.component` to `~/Library/Audio/Plug-Ins/Components/` and immediately call `auval`. Hosted GitHub macOS runners' `AudioComponentRegistrar` does not pick up the new bundle reliably, so auval returns `Cannot get Component's Name strings / Error -50`, the Test step exits non-zero, and the entire sign / notarize / publish pipeline silently fails. This was the failure mode of the 30+ consecutive sign-and-release runs preceding v0.41.0. The validation gates are owned by `validate.yml` on PR; do not duplicate them into the release workflow. `tools/scripts/test_release_workflow_test_step.py` (wired into `workflow-lint.yml`) is the regression test that prevents reintroduction.

**Tag safety:** the auto-release workflow is idempotent-strict — if a tag already exists pointing at a different SHA, it fails loudly rather than overwriting. See `docs/guides/versioning.md` for the manual recovery recipe.

**Shared-source priming is retry-wrapped (#1375):** every `ensure_shared_git_source` call in `setup.sh` runs through `ensure_shared_git_source_with_retry` (3 attempts, 5s/10s/20s backoff, scrubs the partial cache target between attempts). Motivated by v0.74.0 + v0.74.1 release-cli runs both dying on `windows-arm64` mid-`Priming shared Yoga source cache` with exit 127 — a transient command-not-found on a Windows shell wrapper. The retry happens at the WRAPPER level, not inside `ensure_shared_git_source`, because that function uses a `set -e` subshell which a 127 tears down before any inner retry can engage. Override attempts via `PULP_PRIMING_RETRY_ATTEMPTS=N`.

**Per-tag release-cli watchdog (#1375):** `.github/workflows/release-cli-watchdog.yml` triggers on `workflow_run` for `release-cli.yml`. It resolves the tag from `head_branch`, queries the GitHub release for `pulp-sdk-*` and `pulp-{darwin,linux,windows}-*` assets via `gh release view`, and opens a per-tag tracker on three failure shapes: `run_failure`, `success_with_missing_assets` (the v0.74.0 pattern — release exists with only plugin .pkg files from `sign-and-release.yml`), `no_release` (the v0.74.1 pattern — `release-cli`'s `release` job never ran because `build-cli`/`smoke-cli` failed). The tracker's body suggests `gh workflow run release-cli.yml --ref vX.Y.Z -f version=vX.Y.Z` to backfill, and auto-closes when the SDK assets land. This is documented as Layer 2b in `docs/guides/release-watchdog.md`.

**`RELEASE_BOT_TOKEN` is required for the auto-release chain to fire.** Without it, auto-release silently degrades — tags get created via `GITHUB_TOKEN` but GitHub doesn't trigger workflows on `GITHUB_TOKEN`-pushed tags, so `release-cli.yml` and `sign-and-release.yml` never run and no GitHub Release appears. Run `pulp doctor` to check; if missing, follow the "One-time setup" section in `docs/guides/versioning.md`. `pulp pr` will also print a heads-up before pushing the PR if the secret isn't present.

## Coverage workflow (`#566` Phase 1)

`.github/workflows/coverage.yml` has three jobs:

- `resolve-runners` — shared-helper resolver (`tools/scripts/resolve_runs_on.py`) that picks per-OS runs-on labels in priority order: workflow_dispatch input → `PULP_COVERAGE_<OS>_RUNS_ON_JSON` repo variable → hard-coded default (`ubuntu-latest` / `macos-latest` / `windows-latest`). Same pattern as `sanitizers.yml`. Change runner for one OS by setting the repo variable — no workflow edit required.
- `coverage` — matrix over {linux, macos, windows}. Every leg builds with Clang source-based coverage, runs the native test suite, uploads HTML + summary + Cobertura artifacts, and pushes to Codecov with exactly one per-OS flag (`os-linux`, `os-macos`, `os-windows`). The Linux leg also installs `coverage.py >= 7.10`, runs `tools/scripts/run_python_coverage.py` for `tools/scripts/**`, `tools/deps/**`, and `tools/local-ci/**`, uploads the Python HTML + summary + Cobertura artifacts, and includes `build-coverage/python/coverage.python.xml` in the same Codecov upload. The macOS leg additionally runs `tools/scripts/run_swift_coverage.py`, stages `build-coverage/apple/coverage.apple.json`, uploads the Apple summary artifact, and includes that JSON in the same Codecov upload. Subsystem / platform / surface slicing comes from `codecov.yml`'s `component_management` path globs, not from a multi-flag CSV on the upload step. Has `continue-on-error: true` on the heavy coverage steps and `fail-fast: false` on the matrix — a flake on any one OS never cancels the others and never blocks a merge.
- `coverage-diff-gate` — downloads all three OS Cobertura artifacts (`coverage-cobertura-${sha}` for Linux, `coverage-cobertura-macos-${sha}`, `coverage-cobertura-windows-${sha}`), merges them with `tools/scripts/merge_cobertura.py` (taking `max(hits)` per `(filename, line)`), then runs `diff-cover --fail-under=75` against `origin/<base>` on the merged XML. Hard-fails the PR when the global diff-coverage floor is missed. The job still renders and upserts the diff-coverage PR comment via `tools/scripts/coverage_diff_comment.py` even on failure, and it also runs the per-tier gate (`tools/scripts/coverage_tier_check.py`) in advisory mode against the same merged XML.

Gotchas:

- **Fork PRs**: the comment-upsert step has an `if:` guard skipping forks because `GITHUB_TOKEN` is read-only on fork heads; otherwise the comment step would hard-fail with 403 after the gate result is already known.
- **The comment renderer is unit-tested.** When touching `tools/scripts/coverage_diff_comment.py`, run `python3 tools/scripts/test_coverage_diff_comment.py` locally — the workflow also runs it as a pre-flight fixture check so a regression fails fast.
- **The Python tools lane is still Linux-only, but no longer scripts-only.** Today it measures `tools/scripts/**`, `tools/deps/**`, and `tools/local-ci/**`, uses coverage.py's subprocess patching so spawned scripts count, and uploads only on Linux. Python elsewhere (for example top-level `tools/*.py`, `tools/packages/**`, repo-root scripts) is still out of scope until a follow-up expands the surface again.
- **Install Python coverage tooling in a venv, not with `pip install --user`.** Namespace/self-hosted Linux runners can enforce PEP 668 (`externally-managed-environment`), which breaks the coverage lane before `tools/scripts/run_python_coverage.py` even starts. The workflow now creates `build-coverage/python-venv` and runs the Python coverage script through that interpreter; keep future edits on that pattern.
- **The Apple Swift lane is source-only on the macOS leg.** `tools/scripts/run_swift_coverage.py` stages SwiftPM's Codecov JSON for `apple/Sources/PulpSwift/**`; `apple/Tests/**` and generated `apple/.build/**` paths are ignored in `codecov.yml` so the Apple component reflects package sources rather than the test harness. iOS-only files that compile out of the macOS SwiftPM build remain out of scope on this first pass.
- **Adding a new core subsystem** means adding or adjusting the `component_management` path entry in `codecov.yml` and documenting it in `docs/guides/coverage.md`. The upload step itself should still stay at one per-OS flag per upload — Codecov rejected the older "20 flags per upload" shape.
- **Per-OS coverage (Phase 1 PR 4)**: each matrix leg tags its Codecov upload with an OS flag so `host AND os-windows` answers "what fraction of `core/host` is exercised when tests run on Windows?" — a different question from `host AND windows` (which is "what fraction of `core/host/**/windows/` shim files are covered at all"). Cross-OS unions of the same file happen at the Codecov flag layer, NOT via `llvm-profdata merge` (not architecture-portable — see planning decision doc §7).
- **Windows coverage uses Clang, not MSVC.** `tools/cmake/PulpInstrumentation.cmake` rejects MSVC because `/fsanitize-coverage` and llvm-cov emit incompatible profile shapes. The Windows matrix leg adds `C:\Program Files\LLVM\bin` to PATH and builds with clang++; the `windows-msvc-release-gate` job in `build.yml` keeps the MSVC release-path green separately.
- **diff-cover consumes a merged XML, not the per-OS ones.** It's a single-XML tool — running it against three XMLs would produce three PR comments for the same metric with slightly different numbers, more noise than signal. The merge happens once in the job (`merge_cobertura.py`, max-hits-per-line union) so the gate sees a cross-platform view while diff-cover still emits one comment. Earlier the gate read only the Linux artifact and silently skipped Apple-only / Windows-only files (pulp#635). Local `scripts/run_coverage.sh` still produces a single per-host XML; a local diff-cover invocation against that has the original silent-skip and is best treated as a sanity check, not the authoritative gate.
- **Global vs per-tier enforcement**: `diff-cover --fail-under=75` is already required. The per-tier gate is still `continue-on-error: true` while the tier definitions soak; don't silently flip that to required without updating `docs/guides/coverage.md` and the issue trail.
- **Don't `|| ...` the `merge_cobertura` step in `coverage-diff-gate`.** The script uses a DEDICATED exit code (2 = `EXIT_ALL_INPUTS_MISSING`) for the intentionally-tolerated "every input XML missing or empty" case; the diff-cover step then renders the no-XML fallback. Any other non-zero exit (1 = real error: parse failure, script bug, IO error) MUST fail the gate — otherwise a corrupted artifact silently bypasses the required 75% diff-coverage check. Codex P1 reviews on both PR #654 (original `|| echo` shape masked everything) and PR #660 (collapsing rc==1 into the tolerated case let `xml.etree.ParseError` slip through) drove the current shape. The workflow branches on the exact code with `if rc -eq 0` / `elif rc -eq 2` / `else fail`. The script's `EXIT_ALL_INPUTS_MISSING` constant + the workflow's literal `2` are paired — change them in lockstep, and add fixture-tests in `test_merge_cobertura.py` if you alter the contract.
- **Local mirror of the diff-cover gate.** `tools/scripts/local_diff_cover.sh` runs the same `diff-cover --fail-under=$THRESHOLD` flow CI runs, so coverage-only failures don't cost a 20-min CI roundtrip. The threshold + filters are read from `tools/scripts/coverage_config.json` — both the workflow's diff-cover step and the local script consume that file, so editing the JSON in one place keeps CI + local + the pre-push hook in lockstep. Bypass with `PULP_SKIP_DIFF_COVER=1` for workflow-only or doc-only PRs. The Claude Code `/coverage-diff` slash command and `pulp coverage diff` CLI subcommand are thin wrappers over the same script. The pre-push hook runs the script advisory-by-default; `PULP_ENFORCE_PREPUSH_DIFF_COVER=1` upgrades to a hard block. Test coverage in `tools/scripts/test_local_diff_cover.py` includes an anti-drift gate that fails if a future edit hardcodes `--fail-under=NN` back into `coverage.yml`.
- **`diff_cover_excludes` pattern + flag-shape contract** (PR #1005, learned the hard way). diff-cover's `--exclude` is `nargs='+'` with default action — repeated `--exclude=foo --exclude=bar` keeps only the LAST entry. AND its matching is fnmatch against (a) the file's basename and (b) its absolute path; a literal relative path like `tools/cli/cmd_loop.cpp` matches NEITHER and is a silent no-op. So entries in `coverage_config.json` MUST be a basename (`cmd_loop.cpp`) or a glob (`**/cmd_loop.cpp`), and both `local_diff_cover.sh` and `coverage.yml` MUST splat them under a SINGLE `--exclude val1 val2 ...` flag (NOT a per-entry `--exclude=PATH` loop). The previous shape was silent-broken since #919; a new exclude (scanner_clap.cpp) on PR #1005 surfaced the latent bug because it was a 2-entry config that suddenly mattered. Don't introduce a 3-entry config without re-checking that the splatted form still works.
- **`merge_cobertura.py` normalises Windows backslash paths and applies `COVERAGE_IGNORE_REGEX` itself.** Two sneaky bugs found together on PR #660 by walking the actual merged XML: (1) the Windows cobertura emits filenames with backslash separators (`core\\format\\src\\clap_adapter.cpp`), Linux/macOS use forward slashes — without normalisation the merge stores them as TWO files and diff-cover matches the backslash variant against the git diff (which uses forward slashes), finding 0 hits and silently reporting 0% on cross-platform code that was actually exercised on Linux. (2) The Windows leg was leaking ~250 `test\*` entries into the cobertura because run_coverage.sh's `COVERAGE_IGNORE_REGEX` matches `/test/` only — backslash paths slipped past. The merge now normalises slashes AND mirrors the same exclude regex (`tools/scripts/merge_cobertura.py::_IGNORE_RE`) so the gate's view is consistent regardless of which OS produced an artifact. Keep the regex in lockstep with `scripts/run_coverage.sh::COVERAGE_IGNORE_REGEX`.
- **Install PyYAML before any step that imports it.** `tools/scripts/test_coverage_tier_check.py` calls `ctc.load_targets()` which imports `yaml`, so the `Install PyYAML` step in `coverage.yml` must run BEFORE both the fixture-tests step and the per-tier gate step. Issue #900 caught the original ordering where the install ran after the test, so runners without preinstalled PyYAML hard-failed the required coverage job. If you add another script under `tools/scripts/` that imports `yaml` and gets wired into a workflow, make sure the PyYAML install step precedes every step that runs it.
- **Every first-party source must classify into exactly one tier (#1056).** `ci/coverage-targets.yaml` tier globs are silent no-ops if a new source path falls outside every tier — it inherits the looser global 75% floor instead of its intended tier. The `TierCoverageCompleteness` cases in `tools/scripts/test_coverage_tier_check.py` lock this in (every tier matches at least one file; every first-party source under `core/`, `tools/`, `apple/`, `android/`, `inspect/` lands in exactly one tier). Non-instrumented surfaces (`apple/**.swift`, `android/**.kt`, `apple/Package.swift`) classify under `infrastructure` for audit-completeness; the `is_instrumented_source` filter in `coverage_tier_check.py` keeps them out of the score so they don't bias the per-tier number.

## IWYU advisory gate (`#594` Phase 2)

`.github/workflows/iwyu.yml` runs include-what-you-use on the Linux Clang lane to catch transitive-include bugs before they reach the cross-platform matrix. Three incidents on 2026-04-21 (#540 `<memory>`, Slice 4 `<atomic>`, #593 `<algorithm>`) triggered this gate.

- **Advisory until 2026-05-05** — `continue-on-error: true`. PR annotations appear inline on the diff; merges are not blocked.
- **Linux Clang only** — macOS libc++ hides the bug class (false negatives); MSVC is not Clang. Don't attempt to extend it to those runners.
- **Scope** — PR events analyze only files changed vs `origin/<base>`; push-to-main events run a full repo scan and upload the raw IWYU output as an artifact so we can track FP trends.
- **The parser is unit-tested.** When touching `tools/scripts/iwyu_annotate.py`, run `python3 tools/scripts/test_iwyu_annotate.py` locally — the workflow runs it as a pre-flight fixture check so regressions fail fast without burning the build.
- **Mappings** — `.iwyu-mappings.imp` at the repo root maps CHOC amalgamated headers and libstdc++ detail paths to the canonical public include. Prefer fixing the code (adding the missing include) over adding a new mapping.
- **Flip to blocking** (Phase 3 of #594) requires FP rate < 5% across a one-week window. On the flip PR, edit `continue-on-error` to `false`, update `docs/guides/iwyu.md`'s "Advisory until" line, and reference the flip in the PR body. Do not close #594 until the blocking gate has held for a week.

See [docs/guides/iwyu.md](../../../docs/guides/iwyu.md) for the full contributor-facing write-up.

## PEP 668 + Namespace runners

Namespace's runner image is PEP-668-strict: `pip install --user <pkg>` fails with `error: externally-managed-environment` unless you also pass `--break-system-packages`. The github-hosted ubuntu-latest image tolerates `--user` without the flag, so this regression only surfaces after a workflow's matrix routes its Linux leg through Namespace via a `PULP_*_RUNS_ON_JSON` repo variable.

When you add a new `pip install --user` step to a workflow that may run on Namespace, ALWAYS include `--break-system-packages`. Same applies to any virtualenv-less Python helper installed inline at workflow time. If the workflow uses a hard-coded `runs-on: ubuntu-24.04` (e.g. `coverage-diff-gate`), the flag isn't required because GH-hosted runners aren't PEP-668-strict — but adding the flag is harmless and future-proofs against a later Namespace migration.

Symptom (on Namespace) when you forget:

```
error: externally-managed-environment
× This environment is externally managed
╰─> To install Python packages system-wide, try apt install python3-xyz, ...
```

Followed by cascade-skipped downstream steps (default `if: success()`) and a "coverage.python.xml is missing" hard-fail in the validation step. Coverage Linux ran into this when it migrated to Namespace via `PULP_COVERAGE_LINUX_RUNS_ON_JSON` (PR #676 → #677).

## SignalGraph Phase 0 learnings (PR #153)

Gotchas surfaced while landing the four-phase SignalGraph follow-up:

- **AudioUnitSDK 1.4 uses `std::expected` (C++23).** Targets that `#include`
  AUSDK headers need `set_target_properties(<target> PROPERTIES CXX_STANDARD 23)`
  at the per-target level. `target_compile_features(<target> PUBLIC cxx_std_23)`
  alone is **not** enough when `CMAKE_CXX_STANDARD=20` is set at the repo
  root — CMake 3.24's policy makes CXX_STANDARD authoritative over feature
  requirements. Apply to both the `ausdk` target and every consumer
  (`pulp-format`, per-plugin `${target}_AU`). Symptom: GH-hosted mac fails
  with "no template named 'unexpected'"; local Xcode mac builds fine
  because Apple's libc++ is ahead. Linux/Windows are unaffected because
  they don't touch AUSDK.

- **`std::atomic<std::shared_ptr<T>>` needs C++20 libc++ which our
  toolchain doesn't ship.** The workaround is the deprecated
  `std::atomic_load_explicit(&shared_ptr_var, order)` /
  `std::atomic_store_explicit(&shared_ptr_var, value, order)`
  free-function overloads. These still work everywhere we build and
  preserve acquire/release semantics. Revisit when libc++ catches up.

- **Catch2 `REQUIRE` inside a `std::thread` body terminates the process.**
  The REQUIRE throws and std::thread's dtor calls std::terminate when
  unwinding across the thread boundary. For concurrency tests, use an
  `std::atomic<int>` failure counter from the worker and assert on the
  main thread after join.

- **GH-hosted macOS vs local mac for upstream SDK issues.** When an
  upstream SDK (AUSDK, VST3, …) breaks only on `macos-latest` while the
  exact same code builds on a developer's Xcode, that's an Apple clang
  version mismatch. Options: (a) pin the SDK to a known-good commit,
  (b) set CXX_STANDARD per target, (c) `gh pr merge --admin` if
  Linux+Windows Namespace are green and local mac validated. Don't
  chase GH-hosted mac issues on the PR branch — fix upstream or admin-merge.

- **FetchContent threejs clones hang on some macs.** The threejs git
  clone inside CMake's FetchContent step has hung indefinitely several
  times during fresh configures. Mitigations: reuse an existing
  configured build dir; `rm -rf build/_deps/threejs-*` then build only
  the targets that don't need it (e.g., `pulp-host`, `pulp-test-host`);
  or set `-DPULP_ENABLE_GPU=OFF` to bypass the threejs fetch entirely.

- **Fresh worktree cmake configure is expensive (~15+ min)** because every
  FetchContent dep re-populates. Reuse strategy: `git checkout -B
  feature/<new-phase> origin/main` on an already-configured worktree to
  inherit the populated `_deps/`. Saves ~70% on per-phase bootstrap.

- **Skill-sync + version-bump CI gates run on every push.** After each
  push that touches `tools/cli/`, `core/host/`, `.agents/skills/`,
  you'll likely need to (a) append a new bullet to `hosting/SKILL.md`
  or `cli-maintenance/SKILL.md`, and (b) run
  `python3 tools/scripts/version_bump_check.py --mode=apply` to update
  `CMakeLists.txt` + `CHANGELOG.md`. The gate reports "SDK X.Y.Z ✓
  bumped" when satisfied.
- **Android/Kotlin coverage lives in `android.yml`, not `coverage.yml`.**
  The dedicated `android-kotlin-coverage` job provisions Java + the
  Android SDK/NDK, runs `:app:testDebugUnitTest` plus
  `:app:jacocoDebugUnitTestReport`, uploads the JaCoCo artifacts, and
  sends the XML to Codecov. Keep it separate from the Clang-based
  `coverage.yml` matrix — Android coverage is a Gradle/SDK lane, not a
  native profraw lane.

- **Release-time workflows must declare `permissions: contents: write`.**
  Both `release-cli.yml` and `sign-and-release.yml` write to the
  GitHub Releases API (create the release, upload artifacts, fetch
  generate-release-notes content). Without an explicit job-level
  permissions block they inherit a read-only `GITHUB_TOKEN` on
  `push: tags` events and the `Create GitHub Release` step fails with
  `Resource not accessible by integration` — silent release failure
  that lost ~30 sign-and-release runs across v0.20.x → v0.41.0. See
  `ship` SKILL.md § "`sign-and-release.yml` must declare …" for the
  full gotcha; pulp #720 + #724 for the history. When adding a new
  release-time workflow, add the same block.

### Shipyard-drift detection — pre-push hook logs push origin (pulp #1406)

`.githooks/pre-push` writes every push to `.git/.shipyard-drift-log`
(tab-separated: timestamp, branch, sha, origin) so we can audit when
PRs went up via `shipyard pr` (the canonical full-validation path)
versus a direct `git push` (which silently bypasses skill-sync,
version-bump, diff-coverage, and SSH-host validation, turning CI
into the discovery channel).

**Origin signals** (any one marks the push as supervised):
- `SHIPYARD_PR_RUNNING=1` — set by shipyard's wrapper when it
  invokes git push internally. Upstream feature request open at
  the shipyard CLI repo to make this canonical.
- `PULP_VIA_SHIPYARD=1` — user-set fallback marker for supervised
  direct pushes (e.g. inside a `shipyard ship` retry, or when
  using `git push` deliberately under shipyard tooling that
  doesn't expose the env var yet).

**Behavior**:
- Push proceeds either way (escape hatches need to keep working).
- When neither var is set, hook prints a loud warning with the
  recovery checklist (rate-limit / shipyard-bug / SSH-down).
- The drift log is append-only and gitignored.

**When to suppress the warning** (acceptable temporary fallback):
1. GraphQL rate limit exhausted — verify with
   `gh api rate_limit --jq .resources.graphql.remaining` and
   note the reset time.
2. Shipyard tool itself fails — file an issue at the shipyard CLI
   repo, link it in the PR description.
3. SSH host unreachable — prefer `shipyard pr --skip-target NAME`
   (deliberate skip) over direct push.

In all three cases, set `PULP_VIA_SHIPYARD=1` on the direct-push
command to record the push as supervised AND suppress the warning.

After the obstacle clears, resume `shipyard pr` on the next PR.
