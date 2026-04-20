# Release Watchdog

Three layers of protection against silent release failures. All three use
only standard, agent-agnostic tooling (GitHub Actions, `gh` CLI,
`yamllint`, `actionlint`, Python) — any contributor or automation
(Claude, Codex, a human) can understand and invoke them.

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
| 1. Workflow lint | PR review | bad YAML / bad `uses:` / bad shell | seconds (pre-merge) |
| 2. Auto-release watchdog | `workflow_run` completion | runtime failure (any cause) | 1-2 minutes |
| 3. Cadence check | `schedule` every 30 min | any outcome drift — even unknown-unknowns | ≤45 min |

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

## Manual override

All three layers honor the standard `Release: skip reason="..."`
trailer already documented in `CLAUDE.md` — the skip flag makes the
auto-release step decline to tag, Layer 2 treats the workflow run as
a normal success, and Layer 3 sees no VERSION change so never fires.

## Follow-up hooks (optional, not required)

A future enhancement could call the same tools from `.githooks/pre-push`
so linting fires before a branch even reaches GitHub. Not strictly
needed — Layer 1 in CI is sufficient — but reduces iteration time for
contributors who touch CI workflows frequently.

## Related incidents

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
