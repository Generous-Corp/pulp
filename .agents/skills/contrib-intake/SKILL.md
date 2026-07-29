---
name: contrib-intake
description: Maintainer side of an outside contribution — find what has arrived (fork PR, issue, or patches sent out-of-band), adopt it into an in-repo branch with authorship intact, review it against the repo's bar, and ship it through the normal merge path. Use when landing work from someone without write access or without the CI fleet.
---

# Taking in an outside contribution

The contributor's half is the [`contribute`](../contribute/SKILL.md) skill. This
is the other half: what you do when their work arrives.

The whole job is **getting contributed commits onto an in-repo branch that can
obtain the required checks**, then treating it like any other PR.

## Why an in-repo branch, and not just merging their fork PR

`main` requires these checks (`gh api repos/Generous-Corp/pulp/branches/main/protection`):

```
macos · Enforce version & skill sync · Build + prove + (owner-gated) deploy
Vellum trusted freeze · Vellum freeze
```

`macos` is posted by the **self-hosted Mac Studios**, and one required check is
owner-gated by name. A pull request from a fork:

- runs with a read-only token and **no secrets**,
- needs manual approval before any workflow starts,
- and would execute contributor code **on your own machines** if approved.

**There is no precedent to rely on** — as of 2026-07-29 this repo has had zero
PRs from a fork, so that path has never once obtained these checks. Do not
discover whether it works while trying to land someone's contribution.

So: a fork PR (or an issue, or a bundle in a message) is a fine **delivery**
mechanism. It is not the thing you merge. Adopt it into `contrib/<topic>` in
this repo and ship that.

## Before anything else: two rules that come before every step below

**Rule 0 — read the whole diff before you run anything from the patched tree.**

`contributor_check.sh` executes `gates.sh`, which executes other scripts under
`tools/scripts/` **from the tree you just applied their patches to**. So "just run
the check first" is arbitrary code execution on your machine, before any human has
read a line. Read the diff first, and read these paths with real attention:

```sh
git -C /tmp/adopt diff <base>..HEAD --stat
git -C /tmp/adopt diff <base>..HEAD -- tools/ hooks/ .githooks/ .github/ '*.cmake' CMakeLists.txt
```

A contribution that only touches `core/**` and `test/**` is the easy case. Anything
touching build, hook, workflow, or script surfaces is the case this rule exists for.

**Rule 1 — the first build of unreviewed code does not happen on a Studio.**

The self-hosted Mac Studios hold the Developer ID signing keychain and the notary
`.p8` (`~/.config/pulp/secrets/`). Build unreviewed contributions in a Tart VM or a
throwaway worktree with no keychain access. You own a VM fleet; this is what it is
for.

**Corollary — never click "Approve and run" on a fork PR.** `PULP_LOCAL_MACOS_RUNS_ON_JSON`
is a repo *variable*, and variables (unlike secrets) do resolve in fork-PR runs, so one
approval dispatches contributor code onto the credentialed Macs. The fork PR is a
**review surface, not an execution surface**. Worth adding as a hard guard on the
self-hosted matrix legs so an accidental approval cannot reach them at all:

```yaml
if: github.event.pull_request.head.repo.full_name == github.repository
```

## 1. Find what has arrived

```sh
# Fork PRs — normally zero; anything here is an outside contribution
gh pr list --repo Generous-Corp/pulp --state open --json number,author,headRepositoryOwner,title \
  --jq '.[] | select(.headRepositoryOwner.login != "Generous-Corp")
        | "#\(.number) \(.author.login): \(.title)"'

# Issues flagged as carrying a contribution
gh issue list --repo Generous-Corp/pulp --label contribution --state open \
  --json number,author,title --jq '.[] | "#\(.number) \(.author.login): \(.title)"'

# In-repo contribution branches already adopted but not yet shipped
gh pr list --repo Generous-Corp/pulp --state open --json number,headRefName,title \
  --jq '.[] | select(.headRefName | startswith("contrib/")) | "#\(.number) \(.title)"'
```

Label incoming work `contribution` the moment you see it, whatever form it took.
That label is the only thing that makes an out-of-band delivery visible on GitHub
at all — a tarball in a chat message is invisible to every query above until
someone files an issue for it.

## 2. Adopt it, with authorship intact

Their commits should stay theirs. `git am` and `git bundle` both preserve author
metadata; the mistake is squashing it away later.

**Import the ref first, from the main checkout, then make the worktree at it.**
Doing it the other way round does not work: `git fetch <src>:<dst>` refuses when
`<dst>` is checked out anywhere (`fatal: refusing to fetch into branch ... checked
out at ...`), and `gh pr checkout --branch` fails when the branch already exists.

```sh
# ── a fork PR
git fetch origin pull/<N>/head:contrib/<topic>

# ── a bundle
git fetch /path/to/work.bundle <their-branch>:contrib/<topic>

# ── then, for either of the above:
git worktree add /tmp/adopt contrib/<topic>          # no -b; the branch exists

# ── a patch series is the one case that creates the branch itself
git worktree add /tmp/adopt -b contrib/<topic> <their-base-commit>
git -C /tmp/adopt am /path/to/patches/*.patch
```

Check attribution survived, **before any rebase** — after one, this range spans
everything main gained since:

```sh
git -C /tmp/adopt log --format='%an <%ae>  %s' <their-base-commit>..HEAD
```

If that shows your name, the attribution is already lost. If you later squash,
carry it forward explicitly:

```
Co-authored-by: Their Name <their@email>
```

**Verify the base they claimed.** If `git am` needs `-3` or conflicts, their base
is not what the handoff says, and their test results were measured against
different code than you are about to land.

## 3. Validate at their base first, then rebase, then re-validate

Do not rebase before you have reproduced anything. Their claims are only
measurable at the commit they measured them on, and after a rebase a failure is
unattributable — their bug or merge skew, with no way to tell which.

```sh
# 1. at their stated base: reproduce what the handoff claims
cd /tmp/adopt && tools/scripts/contributor_check.sh <test targets>

# 2. only then move it forward
git fetch origin main && git rebase origin/main

# 3. and run it again — a new failure here is merge skew, not their work
tools/scripts/contributor_check.sh <test targets>
```

Read the handoff for per-commit warnings before you judge an intermediate state:
a series can legitimately have the suite red partway through and green at the tip,
and the handoff is where they say so.

## 4. Review it, then execute their gaps list

Run the check (Rule 0 first, and per Rule 1 not on a Studio) and read their
**"What I could not do"** section against it. The check tells you what is
mechanically wrong; their gaps list tells you what nobody has verified yet. A
contribution whose gaps list is empty deserves *more* scrutiny, not less.

**That list is your to-do list, not a disclaimer.** They were told not to attempt
these, which means the work still exists and it is now yours:

```sh
tools/scripts/local_diff_cover.sh <targets>        # the gate they most often cannot run
ctest --test-dir build --output-on-failure         # beyond their targeted suites
tools/testing/daw-smoke/reaper_smoke.py            # view / editor / format-adapter changes
```

Honor explicit requests in the handoff — "run the full `pulp-test-<x>` suite
before landing", "this needs host-in-the-loop verification". They are telling you
where they know the risk is.

Expect path-based gates to fire on files they barely touched — a `widget_bridge.hpp`
edit demands compat-doc updates and a specifically-named test file. Decide whether
the gate is meant literally here; that judgment is yours, not theirs, and it is
the most common thing they will have flagged and left alone. If they left a skip
trailer with a reason, that is the intended mechanism, and the reason is what you
are agreeing or disagreeing with.

## 5. Split before shipping

Take their suggested split seriously — they know which commits are independent.
Independent bug fixes in shared layers should land on their own and not wait
behind anything that changes how something sounds. One PR per concern, in
dependency order.

## 6. Ship it normally

```sh
cd /tmp/adopt && shipyard pr --base main
```

**Do not add `PULP_SKIP_DIFF_COVER=1` out of habit.** That is the docs-only
recipe. Diff coverage is usually the one gate the contributor could not run, it
is not among the required checks, and you are the only person who will ever
measure it — so skipping it here means contributed C++ reaches `main` with its
coverage never measured by anyone. Skip it only for a genuinely docs-only
contribution, and say so.

Version bumps are yours, not theirs — `version-at-land` assigns them post-merge,
and the contributor was told not to touch version files. If `shipyard pr` asks
for a bump, that is the normal flow, not something wrong with their patch.

## 7. Close the loop

Tell them what landed, what you changed, and what you dropped. If you rewrote
part of it, say which part and why. The next contribution's quality depends
almost entirely on whether the last one got a real answer.

## Gotchas

- **A fork PR that shows all-green may be green on the wrong checks.** Advisory
  lanes pass for forks while the required self-hosted ones never start. Read the
  required list specifically, not the summary.
- **Never run `git add -A` while adopting.** This repo carries the `planning`
  submodule, and adopting from a worktree is exactly the situation where a stray
  gitlink bump gets staged. Stage explicit paths.
- **Their "tests pass" is real but narrow.** They ran targeted suites on macOS
  only. Linux, Windows, sanitizers, and DAW behavior are all yours.
- **`git am` failing on the last patch of a series leaves the earlier ones
  applied.** `git am --abort` and restart from a clean branch rather than
  hand-fixing the tail.
