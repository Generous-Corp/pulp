# Release Watchdog

Three layers of protection against silent release failures (plus two
PR-time prevention layers — `fix/feat-needs-bump` for #1009 and the
release-path PR gate for #1962). All use only standard, agent-agnostic
tooling (GitHub Actions, `gh` CLI, `yamllint`, `actionlint`, Python) —
any contributor or automation (Claude, Codex, a human) can understand
and invoke them.

## Why three layers

Releases fail silently when a single check covers only one failure mode.
A real incident on 2026-04-20 proved the point: PR #501 merged an
`auto-release.yml` with a YAML-indent bug that caused GitHub to reject
the workflow file at dispatch time. The run surfaced `conclusion=failure`
with **zero jobs** and no logs — which visually looks identical to
"still running." Every auto-release attempt for the next 24h (8 merges,
including the v0.23.1 version bump) failed the same way. Discovery came
only when someone noticed no new GitHub Release had appeared.

Each layer catches a different failure mode:

| Layer | Trigger | Failure mode caught | Median detection |
|---|---|---|---|
| 0. release-path PR gate | PR touching release-path files | prebuilt-Skia / link-order / CMake breakage at PR time (#1962) | 5-15 min (pre-merge) |
| 1. Workflow lint | PR review | bad YAML / bad `uses:` / bad shell | seconds (pre-merge) |
| 2. Auto-release watchdog | `workflow_run` completion | runtime failure (any cause) | 1-2 minutes |
| 2b. Release-CLI watchdog | `workflow_run` completion | per-tag missing SDK/CLI assets (#1375) | 1-2 minutes |
| 3. Cadence check | `schedule` every 30 min | any outcome drift — even unknown-unknowns | ≤45 min |
| 3b. Draft-stuck check | `schedule` every 30 min | tag exists, Release stuck as draft (#1962) | ≤90 min |

## Layer 1 — Workflow lint (pre-merge)

**File:** `.github/workflows/workflow-lint.yml`

Runs on any PR that touches `.github/workflows/**` or `.github/actions/**`.
Executes three checks:

1. **`yamllint`** against `relaxed` profile. Catches syntactic errors
   and flags most structural issues.
2. **Structural `yaml.safe_load`** on every workflow file. Dumb-but-decisive:
   catches the #501 class specifically (block-scalar terminated by a
   less-indented content line).
3. **`actionlint`** via the `raven-actions/actionlint` reusable action.
   Catches GitHub Actions-specific issues: unknown `uses:` refs,
   deprecated action versions, shell escaping bugs, etc.

Failure means the PR cannot merge until fixed. Running locally:

```bash
# yamllint
pip install yamllint==1.35.1
yamllint -d relaxed .github/workflows/

# structural parse (catches #501-class bugs)
python3 -c "import yaml, pathlib; [yaml.safe_load(p.read_text()) for p in pathlib.Path('.github/workflows').rglob('*.y*ml')]"

# actionlint (brew / go / prebuilt)
brew install actionlint
actionlint
```

## Layer 2 — Auto-release watchdog (runtime)

**File:** `.github/workflows/auto-release-watchdog.yml`

Triggers on `workflow_run` completion for `auto-release.yml`. Fetches
the run's job count via `gh api` and classifies the outcome:

- `success` — if a tracking issue is open from a prior failure, close
  it with a recovery note
- `job_failure` — one or more jobs failed; open or update tracker
- `workflow_file_error` (0 jobs + failure) — GitHub rejected the file;
  open or update tracker with a dedicated message explaining that
  `gh run view` will not have logs

Tracking issue title: `Auto-release workflow failed — RELEASES BLOCKED`.
One issue, edited in place, auto-closed on recovery — mirrors the #475
close-path pattern used by the orphan-branch and deps-drift sweeps.

## Layer 2b — Release-CLI watchdog (per-tag asset check)

**File:** `.github/workflows/release-cli-watchdog.yml`

Triggers on `workflow_run` completion for `release-cli.yml`. Resolves
the tag from `head_branch`, then queries the corresponding GitHub
release for `pulp-sdk-*` and `pulp-{darwin,linux,windows}-*` assets
via `gh release view`. Three alert classes:

- `run_failure` — `release-cli` concluded `failure`. Most common cause
  on Windows-arm64 is the shared-cache priming flake (#1375, exit 127
  during Yoga priming). Tracker body suggests `gh workflow run
  release-cli.yml --ref vX.Y.Z` to retrigger.
- `success_with_missing_assets` — `release-cli` concluded `success`
  but the release has 0 `pulp-sdk-*` tarballs. Matches the v0.74.0
  pattern where `sign-and-release.yml` published plugin .pkg files but
  `release-cli` failed mid-matrix and uploaded no SDK/CLI artifacts.
- `no_release` — no GitHub release exists for the tag. The `release`
  job in `release-cli.yml` runs only after `build-cli` and
  `smoke-cli` clear, so this means the matrix gated the release out.

Per-tag tracker title (`release-cli failed for vX.Y.Z — SDK/CLI
artifacts may be missing`) so each stranded tag gets its own thread —
backfills via `workflow_dispatch` are independent per tag.
Auto-closes on the next run for the same tag if SDK assets land.

## Layer 3 — Release cadence check (invariant)

**File:** `.github/workflows/release-cadence-check.yml`

Runs every 30 minutes (plus `workflow_dispatch`). Scans `main` commits
in the last 24h that changed `CMakeLists.txt`. For each commit that
actually modified the `VERSION` line, checks:

1. Has the commit been on `main` for longer than the grace window
   (default 15 min, to let auto-release finish)?
2. Does a tag `vX.Y.Z` matching the bumped version exist, pointing at
   this commit or a descendant?

If the grace window has expired and no tag exists → add to findings
and open/update a tracking issue titled `Release cadence: version
bumped but no tag`.

This layer is cause-agnostic. Whether auto-release failed because of a
YAML bug (Layer 1 would catch), a runtime job failure (Layer 2 would
catch), a missing secret (neither of the above might catch), a
forgotten manual step, or a GitHub outage — the invariant fires because
the *symptom* (missing release) appears.

## Layer 3b — Release draft stuck check (invariant, sibling to Layer 3)

**File:** `.github/workflows/release-draft-stuck-check.yml`

Runs every 30 minutes (plus `workflow_dispatch`). Sibling to Layer 3:
where Layer 3 catches "VERSION bump landed, no tag", Layer 3b catches
"tag exists, GitHub Release still `draft: true`". Both are
cause-agnostic, both watch the symptom rather than the cause.

For each Release returned by `gh api releases?per_page=100` that is
within the lookback window (default 14 days), checks:

1. Is the corresponding git tag present?
2. Is the Release flagged `draft: true`?
3. Has the Release been drafted longer than the grace window
   (default 60 min — release-cli + sign-and-release together can take
   30–45 min cold-cache; 60 leaves slack)?

If yes-yes-yes → add to findings and open/update a tracking issue
titled `Release pipeline: tag exists but GitHub Release is draft`.
Auto-closes on the next sweep when every recent tag is published.

**Motivating incident (#1962, 2026-05-12..2026-05-14):** Five tags
(v0.95.0..v0.98.0) were drafted by `sign-and-release.yml` (which
creates `draft: true`) but never promoted to published because
`release-cli.yml`'s linux-x64 leg failed for 5 consecutive runs on a
Skia chrome/m144 link-order bug. The `release` job that flips
`draft: false` and uploads the SDK assets is gated on the matrix
being green, so it skipped each time. Existing layers were quiet:
`auto-release.yml` and the cadence check both saw "tag exists" and
moved on; the per-tag release-cli watchdog (Layer 2b) opened
trackers but those got buried in the issue list and the actual user-
visible signal — `https://github.com/danielraffel/pulp/releases`
showing 5 "Draft" rows — wasn't surfaced anywhere actionable.

## release-path PR gate (pre-tag prevention, issue #1962)

**File:** `.github/workflows/release-path-pr-gate.yml`

Sibling to fix/feat-needs-bump. fix/feat-needs-bump catches "user-
facing change merged without a bump"; the release-path PR gate
catches the bigger structural gap that #1962 surfaced: **the
release-build path is never tested at PR time.**

PR `build.yml` builds Pulp from source via FetchContent — it never
runs `tools/scripts/fetch_skia_for_release.py`, never builds the SDK
tarball, never links the prebuilt Skia archives. So every breakage
to the prebuilt-Skia path (chrome/m144 fontconfig undefineds,
SkUnicode core/icu link-order, future Skia bumps that change asset
layout) sails through PR green and only detonates post-tag, when
release-cli.yml is the only workflow exercising that code path.

The PR gate runs the exact `release-cli.yml` build steps —
`fetch_skia_for_release.py`, the `PULP_REQUIRE_GPU_FOR_SDK=ON`
configure, `cmake --build … --target pulp-cli`, and a
`pulp-cpp --version` smoke — for the two platforms that surface
release-path regressions first:

- `linux-x64` — GNU ld is the strictest static-link environment.
  fontconfig undefineds, SkUnicode core/icu order bugs, anything
  involving missing `--start-group`/`--end-group` shows up here
  before macOS or Windows even notice.
- `darwin-arm64` — sanity check that we don't ship a Linux-only
  gate that misses macOS-only regressions (Metal framework drift,
  AppKit symbol changes, etc.).

Triggered only when a PR touches files in the release-path scope:
`tools/scripts/fetch_skia_for_release.py`, `tools/deps/manifest.json`,
`tools/cmake/Find*.cmake`, `tools/cmake/Pulp*.cmake`,
`tools/cli/CMakeLists.txt`, `core/{canvas,render,view}/CMakeLists.txt`,
`CMakeLists.txt`, `release-cli.yml`. Most PRs (view / docs / examples /
plugin) skip this gate entirely so iteration speed is unaffected.

If `release-cli.yml`'s job structure ever drifts from this gate, the
gate is lying. Mirror any structural change to release-cli.yml here
(or refactor both into a shared composite action). The "Mirror
release-cli.yml's Linux deps step verbatim" comment in the workflow
calls this out.

## fix/feat-needs-bump (PR-time prevention, issue #1009)

The watchdog layers above all *react* to a stranded release — a
user-facing fix that merged without a bump. The structural fix is to
catch it at PR time, before the merge ever happens. That lives in
`.github/workflows/version-skill-check.yml` via the
`--require-bump-for-fix-feat` flag on `tools/scripts/version_bump_check.py`.

**What it does:** On PR triggers, parses `${{ github.event.pull_request.title }}`.
If it matches the Conventional Commits prefix `^(fix|feat)(\([^)]*\))?!?:\s`,
asserts that EITHER:

1. A commit in the PR's diff range has subject `chore: bump versions`
   (the canonical subject `pulp pr` writes when a bump was applied), OR
2. A commit in the range carries a top-level
   `Version-Bump: skip reason="..."` trailer (with non-empty reason).

Otherwise hard-fails with a message that suggests both fix paths.

**What it does NOT do:** the per-surface verdict pipeline is unchanged.
Internal-only fixes whose heuristic verdict is "patch (advisory)" still
get a bump injected by `pulp pr` — but if the merge bypasses `pulp pr`
and the bump never lands, this check catches it.

**Motivating incident:** 2026-04-30, PR #1008 (`fix(view): on(id,'click',fn)
auto-wires View::on_click`) merged at 02:36:45Z via `gh pr merge` after
a force-push had short-circuited `shipyard pr`'s version-bump step. The
existing watchdogs all reported green: `auto-release.yml` decided
`SHOULD_TAG=0` and exited successfully (correct outcome for a no-bump
merge). The `release-cadence-check.yml` looks for bumps without tags,
not the inverse. The fix landed on main but consumers couldn't reach it
until the catch-up bump PR (#1011) merged.

### Recommended branch protection

The `version-skill-check` GitHub workflow runs the new check on every
PR, but `gh pr merge` and admin-merge paths can bypass non-required
checks. To make the check load-bearing, add it to branch protection on
`main`:

> **Required check:** `Versioning & Skill-Sync / Enforce version & skill sync`
>
> Configure via github.com/&lt;owner&gt;/pulp/settings/branches → branch
> protection rule for `main` → "Require status checks to pass before
> merging" → add `Enforce version & skill sync`.
>
> Alternatively via the API:
> ```bash
> gh api -X PUT repos/danielraffel/pulp/branches/main/protection \
>     --input <(gh api repos/danielraffel/pulp/branches/main/protection \
>                  | python3 -c 'import json,sys; d=json.load(sys.stdin); \
>                                d["required_status_checks"]["contexts"].append("Enforce version & skill sync"); \
>                                print(json.dumps(d))')
> ```

This is **documented but not enforced** — the user will choose when to
flip the protection on. With it enabled, `gh pr merge` will refuse to
merge any `fix:` / `feat:` PR without either the bump commit or the
skip trailer, regardless of admin / squash / rebase merge mode.

### Layer 3 backstop in `auto-release.yml`

If the PR-time gate is bypassed somehow (force-push race, admin merge,
unknown-unknown), `auto-release.yml` has a final backstop step
(`Stranded fix/feat detector`) that runs after the tag-or-not decision.
When `SDK_SHOULD_TAG=0` AND `PLUGIN_SHOULD_TAG=0` AND the merge
commit's subject matches the same `fix:`/`feat:` regex, it:

1. Emits a `::warning::` annotation visible in the workflow run UI.
2. Opens a tracking issue titled `release: stuck — fix/feat merged
   without bump (<sha>)` with the `release-stuck` label and
   step-by-step recovery instructions.

The tracker is keyed on the tip SHA so multiple stranded merges produce
distinct issues — each needs its own catch-up bump PR.

## Manual override

All three watchdog layers honor the standard `Release: skip reason="..."`
trailer already documented in `CLAUDE.md` — the skip flag makes the
auto-release step decline to tag, Layer 2 treats the workflow run as
a normal success, and Layer 3 sees no VERSION change so never fires.

`Version-Bump: skip reason="..."` is also honored as a release-skip by
the auto-release.yml guard (pulp #1308 follow-up). Authors use this
trailer for fix/feat changes that legitimately don't bump SDK or plugin
versions — typical cases:

- JS-only changes to `packages/pulp-react/` (versions independently
  via `packages/pulp-react/package.json` + `npm publish`)
- Docs / refactors accidentally typed as `fix:` / `feat:`
- Test-infra changes that mention a fix in their subject

Without honoring this trailer here, the post-merge stranded-fix
detector would fire on every such merge and demand a follow-up bump
PR — even though the author already declared no SDK/plugin bump is
needed. The PR-time gate (`version-skill-check.yml`) already accepts
this trailer for the same reason; the post-merge layer now matches.

The two trailers are still semantically distinct:

- `Release: skip reason="..."` — opt out of *this* release tag (e.g.
  the change is part of a multi-PR series; tag the last one).
- `Version-Bump: skip reason="..."` — declare *no SDK/plugin bump
  needed* (the change is genuinely not user-facing for those surfaces).

In both cases, the auto-release guard now treats them as legitimate
opt-outs and won't synthesize a stranded-fix tracker.

## Follow-up hooks (optional, not required)

A future enhancement could call the same tools from `.githooks/pre-push`
so linting fires before a branch even reaches GitHub. Not strictly
needed — Layer 1 in CI is sufficient — but reduces iteration time for
contributors who touch CI workflows frequently.

## Related incidents

- **2026-05-03 (issue #1375)** — `release-cli.yml` runs for v0.74.0 and
  v0.74.1 both died at the same point on `windows-arm64`: mid-`Priming
  shared Yoga source cache...` with exit 127 (no further log output).
  v0.74.0's GitHub release ended up with only the plugin .pkg files
  (those come from `sign-and-release.yml`); v0.74.1 had no GitHub
  release at all (`pulp sdk install --version 0.74.1` 404'd). No
  watchdog alerted — both Layer 2 (auto-release) and Layer 3 (cadence)
  reported green because auto-release.yml itself ran fine and a tag
  existed for both. Fix: retry-on-failure around every shared-source
  priming call in `setup.sh` (so a transient 127 doesn't strand a
  release), plus a parallel Layer 2b watchdog
  (`release-cli-watchdog.yml`) keyed on per-tag SDK-asset presence.
- **2026-04-30 (PR #1008 → issue #1009)** — `fix(view): ...` merged via
  `gh pr merge` after `shipyard pr` short-circuited its bump step
  (force-push race). `auto-release.yml` saw no version movement and
  exited successfully (`SHOULD_TAG=0`). All three watchdog layers
  reported green because none of them watch for the inverse case
  (success-without-tag after a user-facing merge). Fixed by the
  fix/feat-needs-bump PR-time gate plus the `auto-release.yml`
  backstop step documented above.
- **2026-04-20 (PR #501 → #510)** — YAML indent bug rejected auto-release
  at workflow-file level; all 8 runs in the following day failed
  silently. Layer 1 would have caught this at PR review; Layer 2 would
  have alerted minutes after #501 merged.
- **v0.15–v0.18 MSVC include bug** — release builds silently produced
  unusable artifacts for 24h because no gate verified the produced
  binary actually ran. The cadence check would not have caught this
  (tag was created), but the `feedback_silent_release_failure` memory
  in `~/.claude` documents the shape; a future Layer 4 could smoke-test
  the produced binary.
