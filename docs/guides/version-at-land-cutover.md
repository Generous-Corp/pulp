# Version-at-Land Cutover — flipping the single-writer bump bot to `--push`

This guide is the reviewed, sequenced procedure for turning off the
per-PR version-bump treadmill and turning on **post-merge version
assignment** ("option B"). It is release-critical: a wrong flip can drop
a version silently. Do not flip without the GO/NO-GO checklist at the
bottom passing.

## The problem being solved

Every `fix:` / `feat:` PR hand-writes a version number ABOVE main's
current value into `CMakeLists.txt`, `.claude-plugin/plugin.json`, and
`.claude-plugin/marketplace.json`. That is a **shared counter** — only
one PR can own `main+1`. With N validated PRs queued, each one that
merges bumps the number, and every other open PR now conflicts on the
version line, has to re-merge/rebase, and re-conflicts again the moment
the next one lands. This is the deterministic N-1 version-bump race.

## The model

1. **PRs DECLARE intent, they do not write numbers.** A release-worthy
   PR carries a `Version-Bump: <surface>=<patch|minor|major>` trailer and
   touches **no** version files. The PR-side gate accepts this via
   `version_bump_check.py --accept-intent-trailers` (see
   `version_bump_render.render_report`): a touched surface must EITHER be
   file-bumped, OR carry an intent trailer, OR carry a
   `Version-Bump: skip reason="..."` trailer — otherwise it still hard-fails.

2. **A single writer assigns the number after merge.**
   `.github/workflows/version-at-land.yml` runs on every push to `main`.
   In `--push` mode it recomputes from a fresh `origin/main`, assigns each
   declared surface the next version from main's CURRENT value, commits
   `chore: bump versions` with a `Version-Bump-Applied:` marker, and pushes
   `--ff-only` with retry (`version_at_land.apply_and_push`). No two PRs
   ever contend for the same number — the counter is advanced exactly once,
   in commit order, by the sole writer.

3. **The bot's `chore: bump versions` commit triggers `auto-release.yml`**
   exactly like a PR-side bump does today, so tagging and release are
   unchanged downstream of the bump commit.

### Why the race can't lose or duplicate a version

`apply_and_push` is safe under concurrent post-merge runs because of three
independent properties, each covered by a test in
`tools/scripts/test_version_at_land.py`:

- **`--ff-only` push** (no `--force`): git's default non-fast-forward
  rejection means a second writer that advanced `main` between our
  recompute and our push is *rejected*, never clobbered. (The DELETED
  `intent-bump-on-merge.yml` did a bare `git push origin HEAD:main` with no
  `--ff-only` + retry — a merge during its ~30s window silently discarded
  the bump. That workflow and `apply_intent_bump.py` were removed; never
  reintroduce an unguarded push here.)
- **recompute-per-attempt**: each retry re-syncs to the new tip, where the
  drain range now starts AFTER the winner's `Version-Bump-Applied` marker
  and collapses to empty — so the loser no-ops instead of double-bumping.
- **the `Version-Bump-Applied` marker**: the next drain (and any rerun)
  starts after it, so an already-assigned range is never reprocessed.

### Intent is read `--no-merges`-scoped

`version_at_land.intent_trailers` reads `Version-Bump:` trailers from the
range's **non-merge** commits only (`git_range_trailers(..., no_merges=True)`).
A "Merge origin/main into `<branch>`" re-sync commit can carry a stale
intent trailer that was never meant to declare this range's release
(exactly the `Version-Bump: sdk=minor` sitting on merge commit
`ce17af6ad`); honoring it would silently escalate the version. A PR's real
intent lives on its own commits or the squash commit (single parent), so
`--no-merges` keeps every genuine declaration while dropping the re-sync
noise. The auto-release stranded-fix detector uses the same scoping (see
below). Do **not** change `git_range_trailers`' default (merge-walking) —
the bypass-trailer gates depend on it; use the opt-in `no_merges=` flag.

## Claude plugin marketplace reads main HEAD (staleness window)

`claude plugin marketplace add danielraffel/pulp` tracks the repository's
**default branch (`main`) HEAD** — it reads `.claude-plugin/marketplace.json`
and `.claude-plugin/plugin.json` from main, NOT from a release tag
(`docs/guides/claude-code-plugin.md`; there is no `@ref` pin anywhere in the
install docs). `auto-release.yml` creates `plugin-v*` tags for changelog and
release notes, but the marketplace does not consume them.

Consequence under this model: between a **plugin-surface** PR merging (with
intent, `plugin.json`/`marketplace.json` unchanged) and the `version-at-land`
drain committing the bump, main HEAD's plugin version is briefly STALE — a
user who runs `claude plugin marketplace update` in that window sees the old
number. The window is seconds-to-minutes (the `version-at-land` concurrency
group serializes drains), and it is self-healing. This is strictly better
than today's treadmill, but it is a real behavior change worth stating: the
plugin version on main is *eventually* correct, not *atomically* correct at
merge. If atomic plugin versioning is ever required, pin the marketplace to a
tag instead of main HEAD.

## Cutover sequence (do in order)

0. **Land the safe prep FIRST** (this branch): landmine deleted, trailer
   scoping fixed, `apply_and_push` hardened + tested, `--accept-intent-trailers`
   verified, stranded detector taught `PULP_ACCEPT_INTENT_TRAILERS`. All
   dry-run; nothing about releases changes. Merge it normally.

1. **Prove the release-bot commit path.** The bot must push a *commit* to
   protected `main`. It already pushes tags from `auto-release.yml`; a
   commit additionally needs the bot on the branch-protection bypass list,
   and the commit must be SSH-signed via
   `configure_release_bot_ssh_signing.sh` (secret `RELEASE_BOT_SSH_SIGNING_KEY`).
   Verify on a scratch branch before touching `main`.

2. **Drain to zero, then FREEZE.** Announce a short merge freeze. Let the
   queue drain so no open PR still carries a hand-written `chore: bump
   versions` commit. Confirm main's version files are internally consistent.

3. **Apply the one-time straggler rule** (below) to any PR that is already
   in flight and still carries a bump commit.

4. **Flip the gate to accept intent.** In
   `.github/workflows/version-skill-check.yml`, add `--accept-intent-trailers`
   to the `version_bump_check.py` invocation, and switch Shipyard / `pulp pr`
   to emit `Version-Bump: <surface>=<level>` trailers instead of applying
   file bumps. (Surface bump levels are already derived by
   `version_bump_check`'s heuristic; the change is "declare, don't write".)

5. **Flip `version-at-land.yml` to `--push`** — the one-line diff in the
   GO/NO-GO section. Give the job `contents: write`, add the SSH-signing
   step (as `post-tag-sync.yml` does), and add a recursion guard is NOT
   needed (the `Version-Bump-Applied` marker makes the bot's own commit a
   no-op drain).

6. **Teach the stranded detector.** In the `Stranded fix/feat detector` step
   of `.github/workflows/auto-release.yml`, export
   `PULP_ACCEPT_INTENT_TRAILERS=1`. This makes `classify-unreleased-range`
   treat a pending authored intent as covered (not "consumers are stuck"),
   so every intent-carrying fix/feat merge does not false-warn. It is
   `--no-merges`-scoped, so a stray intent on a re-sync merge commit still
   correctly strands. **Do this in the SAME change as step 5** — enabling it
   earlier (while `version-at-land` is still dry-run) would blind the
   detector to genuinely stranded fixes, because nothing would be applying
   the intent.

7. **Watch the first N merges.** Confirm each release-worthy merge produces
   exactly one bot `chore: bump versions` commit and one tag, and that
   `release-cadence-check.yml` stays quiet.

### One-time straggler rule (in-flight PRs)

At the moment of the flip, a PR that was opened before the flip may already
carry BOTH a hand-written `chore: bump versions` commit AND (after a rebase)
an intent trailer — which would **double-bump** (the PR's file bump lands,
then `version-at-land` assigns another number on top). For every PR still
open across the flip boundary:

- **Strip the hand-written bump.** Drop the `chore: bump versions` commit and
  the version-file edits from the branch, and add the
  `Version-Bump: <surface>=<level>` trailer instead. `shipyard pr` /
  `pulp pr` post-flip does this automatically; a manually-managed PR needs it
  done by hand.
- **If a PR must keep its file bump** (e.g. it merges during the freeze,
  before step 5), let it bump normally and add `Version-Bump: skip
  reason="pre-cutover file bump"` so `version-at-land` does not assign a
  second number.

The rule exists only for the flip boundary; once the queue has cycled, every
PR is intent-only and there are no stragglers.

## Rollback

The flip is two workflow edits (`version-at-land.yml` back to `--mode dry-run`;
remove `--accept-intent-trailers` from `version-skill-check.yml`; remove the
`PULP_ACCEPT_INTENT_TRAILERS=1` export from `auto-release.yml`) plus reverting
Shipyard to file-bump. Reverting leaves the bot in dry-run — safe. Any PR
already merged intent-only during the live window keeps its bot-assigned bump;
no data is lost by rolling back.

## GO / NO-GO checklist for the `--push` flip

Flip only when ALL are true:

- [ ] Prep branch (landmine delete + scoping + hardening + tests) merged to
      `main` and green.
- [ ] `tools/scripts/test_version_at_land.py` passes on `main`
      (scoping + concurrency proofs).
- [ ] Release bot verified able to push a *signed commit* to protected
      `main` on a scratch branch (bypass list + `RELEASE_BOT_SSH_SIGNING_KEY`).
- [ ] Merge queue drained; no open PR carries a hand-written
      `chore: bump versions` commit (or each straggler handled per the rule).
- [ ] `version-skill-check.yml` flipped to `--accept-intent-trailers` AND
      Shipyard/`pulp pr` emits intent trailers (verified on one real PR).
- [ ] `auto-release.yml` stranded detector exports
      `PULP_ACCEPT_INTENT_TRAILERS=1` in the SAME change as the push flip.
- [ ] `version-at-land.yml` job has `contents: write` + the SSH-signing step.
- [ ] A dry-run of the flipped `version-at-land.yml` on a scratch push shows
      the correct single assignment before enabling `--push`.

### The exact one-line diff that performs the flip

In `.github/workflows/version-at-land.yml`, the `Dry-run version assignment`
step:

```diff
-      - name: Dry-run version assignment
+      - name: Assign versions (single-writer push)
         run: |
-          python3 tools/scripts/version_at_land.py \
-              --base "${{ steps.base.outputs.ref }}" \
-              --head HEAD \
-              --mode dry-run
-          echo "::notice::version-at-land dry-run complete (no files written)."
+          python3 tools/scripts/version_at_land.py --push \
+              --base "${{ github.event.before }}"
```

(`--push` derives the range from a fresh `origin/main` itself and prefers the
`Version-Bump-Applied` marker; `--base` is only the first-run FALLBACK for the
very first drain before any marker exists — `github.event.before` bounds it to
exactly this push. The job's `permissions:` must become `contents: write` and
an SSH-signing step must run before this step — those are part of the same
reviewed flip, not a separate one-liner.)
