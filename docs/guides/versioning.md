# Versioning & Skill-Sync Policy

Pulp versions three surfaces independently:

- **SDK / CLI** — `CMakeLists.txt` `project(... VERSION x.y.z)`.
  Cascades to `PULP_SDK_VERSION` in generated headers and to the
  CLI binary's `pulp --version`.
- **Claude Code plugin** — `.claude-plugin/plugin.json` `version`
  and `.claude-plugin/marketplace.json` `version`.
- **Shipyard pinned binary** — `tools/shipyard.toml`, consumed by
  `tools/install-shipyard.sh`. This is an upstream release we
  consume, not a surface we ship; see [Dependency Update
  Workflow](https://github.com/danielraffel/pulp/blob/main/CLAUDE.md#dependency-update-workflow) for pin bumps.

The first two are **enforced**: PRs that change code in a surface's
trigger paths without bumping its version are rejected before merge.
The third is covered by the existing `tools/deps/audit.py` path and is
outside this guide's scope.

---

## Three-layer gate

```
Layer 1 (fast, per-edit, agent-specific)
    hooks/scripts/cli-plugin-sync.sh, Claude Code PostToolUse hooks
    → advisory "hint" mode output only

Layer 2 (pre-push, agent-agnostic, advisory)
    .githooks/pre-push
    → same scripts, "report" mode, warns by default
    → PULP_ENFORCE_PREPUSH=1 upgrades to hard fail

Layer 3 (PR gate, authoritative)
    .github/workflows/version-skill-check.yml
    tools/shipyard.toml `version-skill-check` stage
    → same scripts, "report" mode, no bypass without commit trailer
```

Every layer calls into the same two Python scripts — `tools/scripts/version_bump_check.py` and `tools/scripts/skill_sync_check.py`. Heuristics live in the scripts; layer differences are only about where and how the exit code is enforced.

---

## The scripts

### `version_bump_check.py`

Answers "which surfaces need a bump for this diff, and are they bumped?" Modes:

| Mode     | Behavior |
|----------|----------|
| `report` | Exit 1 if any surface needs a bump it hasn't gotten. CI + pre-push. |
| `apply`  | Rewrites version files in place for surfaces that need a bump. Called from `pulp pr`. |
| `hint`   | Always exit 0; prints advisory text. Agent PostToolUse hooks. |

Verdict rules per surface, in order:

1. **Path heuristic** picks a default. Public-header edits → minor-required. Internal-only edits → patch-suggested (warning, not hard fail). Non-trivial comment-or-whitespace-only diffs are downgraded to patch-suggested.
2. **Conventional-commit signals** (`feat:`, `fix:`, `BREAKING:`, `!:` in subjects) can *raise* a surface's level — but only if that surface's paths were touched. A plugin-only `feat:` never upgrades the SDK. Reverts (`Revert "..."` subject or `Revert-Of:` trailer) suppress their signal.
3. **Explicit trailer wins** as the ceiling/override:
   ```
   Version-Bump: sdk=major reason="removed PulpFoo::bar"
   Version-Bump: plugin=skip reason="docs-only PR, no plugin behavior change"
   ```
   Per-surface. The trailer applies only to surfaces whose paths were actually touched — it cannot authorize a bump for an untouched surface.

### `skill_sync_check.py`

Answers "did the diff touch paths mapped to a skill, and was that skill's SKILL.md updated?" One skill map — `tools/scripts/skill_path_map.json` — is the single source of truth. If a change touches a skill's paths and the SKILL.md isn't updated, the check hard-fails unless the tip commit carries:

```
Skill-Update: skip skill=<name> reason="mechanical rename, no new lesson"
```

The script's self-check also fails if any directory under `.agents/skills/` lacks an entry in the map — the map is deliberately explicit so it's reviewed alongside skill changes.

---

## Pre-push hook

```bash
# One-time, per-checkout
tools/scripts/install-githooks.sh
# or:
git config core.hooksPath .githooks
```

After that, every `git push` runs both scripts. By default warnings print but don't block — set `PULP_ENFORCE_PREPUSH=1` (CI does this) to upgrade to hard failures. Single-push bypass for emergencies:

```bash
PULP_SKIP_PREPUSH=1 git push
```

---

## CI workflow

`.github/workflows/version-skill-check.yml` runs on every PR to `main` or `develop`. It fetches full history (so `origin/base_ref` is reachable) and invokes the two scripts in `report` mode. Failure blocks merge.

There is deliberately no bypass in CI other than the commit trailers. The audit trail lives in git, not in GitHub labels or PR-body text.

### fix/feat-needs-bump (issue #1009)

On `pull_request` events, the workflow additionally runs
`version_bump_check.py --require-bump-for-fix-feat`. This asserts that
PRs whose title carries the Conventional Commits `fix:` or `feat:`
prefix include either:

- a commit with subject `chore: bump versions` in the diff range
  (the canonical subject `pulp pr` writes when the bump was applied), OR
- a top-level `Version-Bump: skip reason="..."` trailer (with non-empty
  reason) on any commit in the range.

Otherwise the check hard-fails with a message that suggests both fix
paths. The motivating incident (PR #1008, 2026-04-30) merged a
user-facing fix without a bump; the existing per-surface verdict
heuristic classified the diff as patch-suggested (advisory), so nothing
blocked the merge and `auto-release.yml` had nothing to tag.

For full design + recommended branch protection see
[docs/guides/release-watchdog.md](release-watchdog.md#fixfeat-needs-bump-pr-time-prevention-issue-1009).

---

## Shipyard stage

`tools/shipyard.toml` gains a `version-skill-check` stage that mirrors the CI workflow. This keeps `shipyard ship` (the primary merge path) in lockstep with GitHub Actions.

Note: **Shipyard configuration changes** (the `tools/shipyard.toml` file itself, `tools/install-shipyard.sh`, and everything under `.github/workflows/`) are mapped to the `ci` skill in `skill_path_map.json`. That means editing the merge workflow automatically demands a `ci` SKILL.md review — the skill-sync gate catches shipyard-config drift the same way it catches subsystem-code drift.

---

## "Push a PR" — the one-command path

Typing `pulp pr` (or saying "push a PR" / "ship this" / "we're done" to an agent configured with this policy in `CLAUDE.md`) runs the full pipeline:

1. `skill_sync_check.py --mode=report` — hard-fails here if a mapped path is touched without a SKILL.md update. The only reason to bounce back to you is to add a gotcha or a bypass trailer.
2. `version_bump_check.py --mode=apply` — applies the required bump(s) to `CMakeLists.txt` / `plugin.json` / `marketplace.json`, staging them. Appends a CHANGELOG stub.
3. `git commit` — single "chore: bump ..." commit.
4. `gh pr create` — PR body auto-populated with the bump verdict.
5. `shipyard ship` — cross-platform validation + merge on green.
6. On merge, `.github/workflows/auto-release.yml` diffs the version files against the previous push, creates the matching tag(s), and the existing tag-triggered release workflows publish binaries.

Never type `gh pr create` + `shipyard ship` separately. Never run the version-bump scripts by hand unless debugging.

---

## One-time setup: `RELEASE_BOT_TOKEN` secret

The auto-release workflow needs a fine-grained PAT to push tags so that the tag-triggered binary workflows (`release-cli.yml`, `sign-and-release.yml`) actually fire. **Without this secret, auto-release silently degrades**: tags are still created via `GITHUB_TOKEN`, but GitHub Actions deliberately does not chain workflows from `GITHUB_TOKEN`-pushed tags (anti-infinite-loop safety), so the binary release workflows never run and no GitHub Release appears.

Run `pulp doctor` to check whether the secret is configured. If it shows `RELEASE_BOT_TOKEN secret — missing`, set it up:

1. **Generate the token.** github.com → top-right avatar → Settings → Developer settings → Personal access tokens → **Fine-grained tokens** → Generate new token.
2. **Token name:** `pulp-release-bot` (or any descriptive name).
3. **Expiration:** 1 year (mark your calendar to renew).
4. **Resource owner:** the org or user that owns this repo.
5. **Repository access:** Only select repositories → this repo only.
6. **Permissions** (Repository permissions section): **Contents: Read and write**. Leave everything else at the default.
7. **Generate**, copy the token (starts with `github_pat_…`).
8. **Add to repo secrets:** github.com/&lt;owner&gt;/&lt;repo&gt;/settings/secrets/actions → New repository secret. Name: `RELEASE_BOT_TOKEN`. Value: paste the token.

That's it — no code change needed. The workflow already reads `${{ secrets.RELEASE_BOT_TOKEN || secrets.GITHUB_TOKEN }}`. `pulp doctor` will then report `RELEASE_BOT_TOKEN secret — configured ...`. `pulp pr` will also stop printing the heads-up warning before each push.

### Manual fallback when the secret isn't set

The chain still works but requires one manual step per release after the auto-tag appears:

```bash
gh workflow run release-cli.yml --ref v<x.y.z>
gh workflow run sign-and-release.yml --ref v<x.y.z>
```

(Pulp's first auto-released tag, `v0.4.0`, used this fallback before `RELEASE_BOT_TOKEN` was provisioned.)

---

## Release pipeline — how CHANGELOG.md and the Release page stay in sync

Two artifacts have to stay in lockstep after every release: the **GitHub Release page** (shown to users at `github.com/.../releases/tag/vX.Y.Z`) and the repo's **CHANGELOG.md** (shown to developers on main). Both are produced by the same generator so they cannot drift.

**Division of labor:**

- **Shipyard** — pre-merge. Runs `shipyard ship` to validate + merge PRs on green across macOS + Linux + Windows. Stops when the PR lands on main. Does not touch tags, CHANGELOG.md, or the Release page.
- **Pulp's `.github/workflows/auto-release.yml`** — post-merge. On push to main, diffs version files and, if an SDK version moved, creates the `vX.Y.Z` tag. CHANGELOG regeneration is handled separately by `post-tag-sync.yml` (below) so binary builds and docs sync can fail independently.
- **Shipyard's `.github/workflows/post-tag-sync.yml`** — post-tag (installed by `shipyard release-bot hook install`). On tag push, runs `shipyard changelog regenerate` to rewrite `CHANGELOG.md`, commits as `pulp-release-bot` with `[skip ci]` and the three bypass trailers, and pushes back to main (rebase-retry loop handles races).
- **Pulp's `.github/workflows/release-cli.yml`** — post-tag. On `vX.Y.Z` push, builds binaries and creates the GitHub Release with body populated via `shipyard changelog regenerate --release-notes vX.Y.Z` (same generator as `CHANGELOG.md`, so the two cannot drift).

**End-to-end sequence:**

```
pulp pr  (shipyard ship merges the bump PR on green)
       ↓
auto-release.yml  (diffs version files, pushes vX.Y.Z tag)
       ↓
  ┌────────────────────────────────────────┬──────────────────────────────────┐
  ↓                                        ↓                                  ↓
post-tag-sync.yml                    release-cli.yml                  (tag visible on GitHub)
  (shipyard hook)                     (build binaries)
  └── shipyard changelog regenerate   └── shipyard changelog regenerate
      → commit CHANGELOG.md, [skip ci]     --release-notes vX.Y.Z
      → push to main (rebase-retry)    → create GitHub Release body
```

**Single source of truth:** `shipyard changelog regenerate` (shipyard v0.11+). Configured via `[release.changelog]` in `.shipyard/config.toml`:

- No args — rewrites `CHANGELOG.md` from the full tag graph. Called by `post-tag-sync.yml`. Idempotent.
- `--release-notes vX.Y.Z` — prints the one release's markdown (what's new + CHANGELOG anchor + previous-release link). Called by `release-cli.yml`.

Both formats use matching anchors: `<a id="v0140"></a>` in CHANGELOG.md, `#v0140` in the release body link. GitHub's slugifier doesn't interfere because the anchor is explicit.

**Why the split between auto-release.yml and post-tag-sync.yml:** decoupling the tag push from the docs regen means binary builds aren't blocked if the CHANGELOG commit push races against another merge, and vice versa. If `post-tag-sync.yml` fails, run `shipyard changelog regenerate` locally and open a docs PR — no binary impact.

**What breaks if you bypass it:** manually editing `CHANGELOG.md` between releases is fine (the generator is idempotent and picks up your edits on the next regen pass as long as they land in the right release's bullet block). Creating a tag manually via `git tag -a vX.Y.Z && git push origin vX.Y.Z` still fires `post-tag-sync.yml`. The pre-v0.21 in-tree `tools/scripts/regenerate_changelog.py` was deleted when this migrated to shipyard — if you're on an older checkout, rebase before digging in.

---

## Agent parity

Both Claude Code and Codex pick up this policy from `CLAUDE.md`. Codex reads `AGENTS.md` which is a thin pointer at `CLAUDE.md` — the single source of truth for both agents. There is no separate policy file for Codex, and `AGENTS.md` intentionally stays empty so the two never drift.

`codex_hooks` remains an experimental Codex feature (confirmed 2026-04-12 via `codex features list`). A `.codex/hooks.json` may be added as an advisory path later, but CI is the authoritative gate — agent hooks only make failures visible earlier in the loop, not block anything.

---

## Bypassing a check

All three bypass trailers live on the tip commit, never in the PR body. The audit trail must be in git.

| Check          | Trailer                                                   |
|----------------|-----------------------------------------------------------|
| Version bump   | `Version-Bump: <surface>=<patch|minor|major|skip> reason="..."` |
| Skill update   | `Skill-Update: skip skill=<name> reason="..."`           |
| Compat update  | `Compat-Update: skip prefix=<section\|*> reason="..."`    |
| Auto-release   | `Release: skip reason="..."`                              |

A bypass is a recorded admission that the author thought about the rule and decided it doesn't apply. Empty-reason bypasses are rejected.

The compat-update gate uses the same three-layer pattern as the
version-bump and skill-update gates; see [compat-sync.md](compat-sync.md)
for the path map, requirement kinds, and rollout state (#1029 / #1027).

---

## Shipyard-binary pin bumps

When a new Shipyard release drops, the pin lives in `tools/shipyard.toml`
(consumed by `tools/install-shipyard.sh`). Bumping it is a normal
dependency-pin update, not a Pulp-versioning event:

```bash
python3 tools/deps/audit.py --strict --check-upstream --format markdown
# edit tools/shipyard.toml
python3 tools/deps/validate_hosts.py
```

See [CLAUDE.md § Dependency Update Workflow](https://github.com/danielraffel/pulp/blob/main/CLAUDE.md#dependency-update-workflow) for the full procedure. The `ci` skill's path map catches the file change and demands a SKILL.md review.
