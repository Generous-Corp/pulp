# Release Watchdog

How Pulp detects — and now *repairs* — release failures.

The important change: release health is owned by **one reconciler that fixes
things**, not by a fleet of watchdogs that only report. Four reporting watchdogs
(`release-guard`, `release-health`, `release-cli-watchdog`,
`release-draft-stuck-check`) were retired after filing **413 issues in two
weeks** while fixing nothing — recovery was always a human running
`gh workflow run` by hand.

They failed in two distinct ways, both worth remembering before adding another
reporter:

- **Their grace windows were shorter than the pipeline.** They alarmed after
  15/30/45/60 minutes on a pipeline that really takes 70–165+ minutes, so they
  fired on *healthy* releases that were still building. Roughly half of all
  issues filed self-resolved.
- **Dedupe silently broke, so one condition minted issues forever.**
  `release-health.yml` created issues with a `--label release-health` that *does
  not exist in this repo*; the labelled create failed, an unlabelled fallback
  fired, and its dedupe/auto-close lookups (which filter by that label) could
  never match their own issues. It opened a fresh "🚨 Release pipeline DOWN"
  issue every two hours, forever — 69 of them, none closable. Its title also
  embedded an escalating count, so they were not even title-identical. (It had a
  second latent bug: `gh api --paginate` without `--slurp` emits one JSON
  document *per page*, which is not valid JSON — once the repo passed 100
  releases, parsing failed and every tag looked unreleased.)

If you add a watchdog, it must (a) know whether the thing it is judging is still
running, and (b) be able to find its own previous issue. Otherwise it becomes a
firehose.

## The layers

| Layer | Trigger | Failure mode caught | Median detection |
|---|---|---|---|
| 0. release-path PR gate | PR touching release-path files | prebuilt-Skia / link-order / CMake breakage at PR time (#1962) | 5-15 min (pre-merge) |
| 1. Workflow lint | PR review | bad YAML / bad `uses:` / bad shell | seconds (pre-merge) |
| 2. Auto-release watchdog | `workflow_run` completion | auto-release.yml runtime failure (any cause) | 1-2 minutes |
| 3. Cadence check | `schedule` every 30 min | version bumped on main but no tag created | ≤45 min |
| 4. **Release reconciler** | `schedule` every 30 min | **tag exists but never published — and REPAIRS it** | ≤30 min |

Layers 0-3 are prevention and detection. Layer 4 is the only one that changes
release state, and it can only ever drive a release *forward*.

## Layer 4 — Release reconciler (detection AND repair)

`.github/workflows/release-reconcile.yml` + `tools/scripts/release_reconcile.py`.

Every 30 minutes it compares desired state (every recent SDK tag has a published
release) against actual state, and drives the difference to zero by
re-dispatching `release-cli.yml` for any tag that is stuck.

The decision rules — `decide()` in `release_reconcile.py`, unit-tested in
`test_release_reconcile.py`:

| Tag state | Action |
|---|---|
| Published, with every required asset | nothing |
| Published but MISSING assets (at/above the asset-contract floor) | incident — a published release is immutable, so it needs a new patch tag |
| A `release-cli` run is queued/in-progress | **nothing, at ANY age** |
| Younger than the grace window, no run yet | nothing |
| Unpublished, but a NEWER version already shipped | nothing — see "superseded" below |
| No release, no live run, not superseded | re-dispatch `release-cli.yml` (up to 3 attempts) |
| A draft was left behind | re-dispatch — the re-run re-drives the draft to published |
| Retry budget exhausted | ONE incident issue, updated in place, closes on recovery |

**"Superseded" is not the old reaper.** The reaper *cancelled* in-flight runs and
*deleted* drafts for any tag older than the latest published release — destructive,
and racing releases that were merely slow. This rule only declines to **start** a
speculative rebuild of a tag whose users are already served by a newer release. It
never touches release state, and a live run always outranks it. Without it, the
reconciler's first sweep would have re-dispatched twelve superseded tags at once,
saturating the very runners the current release depends on.

**The asset-contract floor** (`ASSET_CONTRACT_FLOOR`) exists for the same reason.
Releases from before the Intel `darwin-x64` pair and `SHA256SUMS` legitimately lack
them; holding them to today's contract would flag a pile of perfectly good historical
releases — a brand-new false-alarm firehose. Raise the floor when the required asset
set changes; never lower it.

Two properties are load-bearing:

**A live run outranks everything, at any age.** A release that has been building
for four hours is *slow*, not stuck. The predecessor of this workflow — a
"supersede reaper" in `auto-release.yml` — kept concluding otherwise and
cancelling in-flight releases, which is how 11 of 18 tags in July 2026 were
destroyed with all their binaries built green.

**It cannot destroy anything.** There is no cancel path and no delete path, and
a test (`NeverDestructive`) asserts the source contains neither. Recovery by
destroying release state is the bug this workflow exists to undo.

Re-dispatch runs `release-cli.yml` from `main` (so it picks up the *current*,
fixed workflow) with `version=<tag>` (so it builds the *tag's* source). It is
idempotent: the finalizer no-ops on an already-published tag.

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

**What it does:** On PR triggers, parses both
`${{ github.event.pull_request.title }}` and the live commit-derived signals
contributed by the PR range. Explicit reverts cancel their target signals;
reverting a revert restores its target signal. If either carries the
Conventional Commits prefix
`^(fix|feat)(\([^)]*\))?!?:\s`, it asserts that EITHER:

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

### Required branch protection

The `version-skill-check` GitHub workflow runs this check on every PR. The
canonical repository makes it load-bearing through branch protection on
`main`:

> **Required check:** `Versioning & Skill-Sync / Enforce version & skill sync`
>
The checked-in intent lives in `.github/rulesets/main-protection.json`; keep the
live ruleset aligned with it. Normal merges then refuse any PR whose title or
commit subjects signal `fix:` / `feat:` without either the bump commit or the
skip trailer, regardless of squash / rebase / merge-commit mode. Administrators
can still bypass branch protection when the live ruleset permits it, so the
post-merge detector remains load-bearing.

The same required check writes an **Expected release tags** run summary for the
PR queue. It predicts SDK and plugin tags from the PR head, fetched tag state,
the repository's one-commit-subject/multi-commit-PR-title squash policy, and
sticky `Release: skip` metadata. Tag creation
remains a post-merge action in `auto-release.yml`; the queue report is
prediction, not a pre-created tag.

### Layer 3 backstop in `auto-release.yml`

If the PR-time gate is bypassed somehow (force-push race, admin merge,
unknown-unknown), `auto-release.yml` has a final backstop step
(`Stranded fix/feat detector`) that runs after the tag-or-not decision.
It applies the same live-signal classifier to the whole pushed range and maps
each signal back to its release surface. That catches plain merge tips,
multi-commit rebases, re-reverts that restore an older `fix:` / `feat:` signal,
and mixed pushes where (for example) a plugin tag does not cover an unbumped
SDK fix. Explicit `Release: skip` and range-wide top-level
`Version-Bump: skip` opt-outs remain silent; a sticky per-surface
`Release: skip` counts as intentional coverage only for that surface when its
bump belongs to the pushed range. The bump commit is the coverage boundary:
signals at or before it are covered, while a later fix remains uncovered.
Recovery also retains independent fix/feat levels per surface and respects an
explicit numeric `Version-Bump: <surface>=<level>` verdict.
The tracker records those exact levels and passes them back through
`--recover-levels`, so covered feature work cannot inflate a later fix recovery.
For multi-commit squashes, the auto-release guard also recognizes exact embedded
source skip-trailer lines before GitHub's co-author footer; that footer prevents
ordinary `interpret-trailers` parsing from seeing the nested source trailers.
For an unbumped live signal, it:

1. Emits a `::warning::` annotation visible in the workflow run UI.
2. Opens a tracking issue titled `release: stuck — fix/feat merged
   without bump (<sha>)` with the `release-stuck` label and
   step-by-step recovery instructions.

The tracker is keyed on the tip SHA so multiple stranded merges produce
distinct issues — each needs its own catch-up bump PR. Unlike version-keyed
release watchdogs, a SHA-keyed tracker cannot be auto-closed from its title
alone because the affected surface is not encoded there. Close it only after
verifying that surface's later tag or published release contains the stranded
commit. Its generated recovery command starts from fetched `origin/main`,
preserves the historical analysis range, and passes the explicitly uncovered
surface list. Current `main` is the version-arithmetic base, while
`--recover-stranded-release` ignores a historical marker-only
`chore: bump versions`; later version movement, a stale marker, or an already
released sibling surface cannot produce a successful no-op or duplicate tag.

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

