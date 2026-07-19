# Self-hosted GitHub Actions runner setup

This guide covers setting up a Mac (or any machine) as a persistent
Pulp CI runner. The payoff is getting sanitizer + build jobs off
GitHub-hosted runners onto your own hardware — usually 4-8× faster,
and free. Related: the runner-selectable mechanism in
[`local-ci.md`](local-ci.md#switching-a-jobs-runner-without-a-code-change)
and issue [#412 step 6](https://github.com/Generous-Corp/pulp/issues/412).

## Who this is for

- Pulp contributors running a dedicated Mac mini / Mac Studio / Linux
  box as a persistent CI target
- Anyone opting into local sanitizer runs via the
  `PULP_SANITIZER_*_RUNS_ON_JSON` repo variables
- Not for ephemeral or shared-dev-laptop runners — the install is
  designed to be left running

## Prerequisites

- macOS 14+ (sanitizers), macOS 15+ preferred (Apple Clang 16+ has
  C++20 P0960 paren-aggregate-init, needed by `core/view/include/pulp/view/lasso.hpp`)
- Xcode installed, with license accepted: `sudo xcodebuild -license accept`
- Homebrew in PATH for the runner user
- A non-root user account dedicated to the runner (don't reuse your
  personal login account if you can avoid it; gate brittle env with a
  fresh `$HOME`)
- git-lfs installed: `brew install git-lfs && git lfs install`

## Install the runner

1. Create a dedicated user (optional but recommended): `sysadminctl -addUser pulpci -fullName "Pulp CI" -password -`
2. Log in as that user, or use `sudo -iu pulpci` to get a clean shell.
3. Follow GitHub's
   [Adding self-hosted runners](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/adding-self-hosted-runners)
   flow. Download the runner to `~/actions-runner/`, run `./config.sh`,
   register against the `Generous-Corp/pulp` repo.
4. Install as a launch agent so it survives reboot: `./svc.sh install; ./svc.sh start`.
5. Add labels so Pulp workflows can target this runner specifically.
   At minimum: `self-hosted`, `macos`, `arm64`, `sanitizer` (or whatever
   repo var you wire up).

## First-run gotchas (real traps we've hit)

### 1. git-lfs hook conflict on second checkout

**Symptom:** `actions/checkout@v5` with `lfs: true` fails immediately:

```
Hook already exists: ~/actions-runner/_work/pulp/pulp/.git/hooks/pre-push
To resolve this, either:
  1: run `git lfs update --manual` for instructions on how to merge hooks.
  2: run `git lfs update --force` to overwrite your hook.
##[error]The process '/opt/homebrew/bin/git' failed with exit code 2
```

**Root cause:** Self-hosted runners don't get a fresh `$HOME` per run.
LFS hooks installed by your shell's `git lfs install` stick around in
`_work/pulp/pulp/.git/hooks/*` and conflict with the checkout action's
own hook setup on the second and subsequent runs.

**Fix (one-time):** On the runner machine, in the repo workspace:

```bash
cd ~/actions-runner/_work/pulp/pulp
git lfs update --force
```

This overwrites the conflicting hook so `actions/checkout@v5` can
merge its own LFS hooks cleanly on the next run.

### 2. Xcode license not accepted

**Symptom:** CMake configure fails immediately with `xcodebuild: error: You have not agreed to the Xcode license agreements`.

**Fix:** On the runner machine, one time:

```bash
sudo xcodebuild -license accept
```

Required once per Xcode upgrade.

### 3. Homebrew not in PATH

**Symptom:** Steps installing `ccache` / `shipyard` / `pluginval` fail
with `brew: command not found`.

**Fix:** Verify the runner service starts with Homebrew's shim on
PATH:

```bash
# On Apple Silicon:
echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zshrc

# Or explicitly for the runner user's launchd environment:
launchctl setenv PATH "/opt/homebrew/bin:$PATH"
```

Restart the runner service after editing: `~/actions-runner/svc.sh stop && svc.sh start`.

### 4. Apple Clang version skew

**Symptom:** Build fails with e.g. `no matching constructor for initialization of 'canvas::Color'` in `lasso.hpp`.

**Root cause:** Apple Clang 15 (macOS 14) lacks C++20 P0960 (paren
aggregate-init). `lasso.hpp` used `Color(100, 150, 255, 40)` which
requires Clang 16+ (macos-15).

**Fix:** Use macOS 15+. If you must stay on macOS 14, the code has
already been refactored to use `Color::rgba8(...)` (see PR #401), so
this should just work now — but watch for future regressions.

### 5. Git config "helpful" Xcode defaults

**Symptom:** `actions/checkout` hangs or misreads files; LFS pointers
don't resolve.

**Root cause:** Xcode's Source Control pane will occasionally set
`core.fsmonitor = rider` or `core.untrackedCache = true` on a global
git config. Both confuse `actions/checkout`.

**Fix:** On the runner user's account:

```bash
git config --global --unset core.fsmonitor
git config --global --unset core.untrackedCache
```

### 6. Running as root (do not)

**Symptom:** brew complains loudly, or CMake writes to paths you can't
reach from your dev user later.

**Fix:** The actions-runner install should be done as a regular user,
not `root`. Re-run `./config.sh` as yourself if you accidentally ran
as root.

## Teardown / rotation

To decommission a runner:

```bash
cd ~/actions-runner
./svc.sh stop
./svc.sh uninstall
./config.sh remove --token <REMOVE_TOKEN>
```

Get the remove token from GitHub → Settings → Actions → Runners → the
runner → Remove.

## Wiring into Pulp's runner-selectable system

Once the runner is up and labeled `self-hosted,macos,arm64,sanitizer`:

```bash
# Route TSan to this runner:
gh variable set PULP_SANITIZER_TSAN_RUNS_ON_JSON \
  --body '["self-hosted", "macos", "arm64", "sanitizer"]'
```

Next sanitizer run picks it up — no code change. See
[local-ci.md § Switching a job's runner](local-ci.md#switching-a-jobs-runner-without-a-code-change)
for the full variable list and precedence rules.

## Troubleshooting

If jobs dispatch to the runner but fail at a step that worked on
GitHub-hosted before:

1. Compare `$HOME` between runners (self-hosted often has Xcode/Homebrew
   state GitHub-hosted images lack).
2. Check `actions-runner/_diag/*.log` for the runner's perspective.
3. For LFS / checkout issues, delete `_work/<repo>/<repo>/.git` and let
   the next checkout reclone from scratch — destructive but fast recovery.
4. Re-run the offending PR's job manually via `gh run rerun <id>`.

Refs: #429, #424, #412 step 6.
