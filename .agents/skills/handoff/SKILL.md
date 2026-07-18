---
name: handoff
description: Coordinate a cross-session or cross-machine handoff — snapshot the open work, write a status doc to the pulp-planning repo on main, and emit a ready-to-paste goal prompt that links it, so a fresh session (often on another machine) can pick up and finish. Use when the user says "coordinate a handoff", "hand this off", "move this work to a new machine", "we're retiring this session", or asks for a status doc plus a goal for someone else to continue.
---

# handoff

Turn "coordinate a handoff" into three artifacts: a **status doc committed to
pulp-planning `main`**, a **simple goal prompt that links it**, and a **list of
open items** for the next session to track. The point is a fresh session — often
on a different machine, with none of this conversation's context — can start cold
and finish the work.

Keep the *goal* short (one or two sentences + a link + the issue numbers).
Put all the detail in the *doc*. A goal the user pastes should fit in a glance;
the doc is where the elaboration lives.

**Carry the original ask forward faithfully — this is the #1 job.** A handoff
exists to hand off *the goal the user actually gave*, not a reconstruction of it.
A handoff most often happens mid-project, and the failure mode is quietly
narrowing the goal to "whatever's left in flight," or paraphrasing it into
something subtly different from what was asked. Guard against both:

- **Rediscover the last-known goal before writing anything.** Prefer the goal as
  it was *captured* — the user's own words. Look, in order, for: a persisted goal
  record (a `planning/*-goal.md` or the "original ask" block of a prior handoff
  doc), the standing objective the session was given (e.g. a Claude Code `/goal`
  or the opening prompt), and the earliest user messages that framed the project.
  Quote it; don't restyle it.
- **If it was never captured, reconstruct it faithfully and then persist it** in
  the doc's "original ask" block, marked as a reconstruction — so the *next*
  handoff rediscovers a real record instead of reconstructing again. Each handoff
  should make the goal more durable, not drift it further.
- **Fidelity over polish — no invented scope.** Do not add scope, sharpen vague
  asks into crisp requirements, or merge in your own opinion of what "should" be
  done. If the ask was "build this planning file until it's all done," the handoff
  says exactly that — not a reworded, expanded brief. A handoff is *translation*,
  not a fresh review: never turn "carry this plan to the new machine" into "re-plan
  the project." When unsure whether a requirement was in the ask, mark it inferred,
  not stated.
- **Source from the original documents.** Point at the plan/spec/issue/goal the
  user actually wrote and quote it; do not substitute your own summary as the
  record. If the original lives somewhere ephemeral (a scratch plan, in-session
  goal text), copy it verbatim into the durable goal record so it survives.
- **Capture honest evolution, kept distinct.** Goals do grow — the user adds scope
  as work proceeds. Capture those additions too, but **separate "original ask"
  from "added later" (with when/where each came from)** so the record stays honest
  and no one can't tell invented scope from real scope. Only include an addition
  the user actually made; never one you think would be good.
- The remaining tasks/issues are how the objective is *tracked*, never a
  redefinition of it. "Do all of X; here's the part still undone" — never "do the
  three tickets that happen to be open."

**Capture the goal at ask-time (the durable fix).** The reliable way to hand off a
goal is to have written it down when it was given. When a user sets a project goal
or standing objective, persist it verbatim to a durable, agent-neutral,
machine-portable place (a planning goal file) so any later session — Claude or
Codex, any machine — can rediscover the *last known goal* without depending on one
CLI's memory or slash-command state. The handoff is the fallback when that wasn't
done; the capture is the fix.

**Reference, don't duplicate.** The doc is a *map to* the work, not a copy of it.
Anything already captured in another artifact — a plan/spec, an ADR, a GitHub
issue, a commit message, a diff, an existing skill — gets **linked by path or
URL**, never re-pasted. Re-leverage the docs the work already produced. A handoff
that restates a tracking issue's item list drifts from it the moment the issue is
edited; one that links the issue stays correct for free. The doc's own content is
only what exists *nowhere else*: the current honest status, which tracker to drive
vs monitor, the working pattern, and the traps this work paid for.

## When to use

The user says any of: "coordinate a handoff", "hand this off to a new session",
"move this work to a new machine", "we're retiring you / this session", "write a
status doc and a goal so someone can finish this", "make an easy handoff."

## Steps

### 1. Recover the original ask (do this first, before anything else)

Find the goal the user actually gave, in the user's own words, per the "carry the
original ask forward faithfully" principle above. Prefer a captured record (a
`planning/*-goal.md`, a prior handoff's "original ask" block, the session's
standing objective / opening prompt); fall back to the earliest framing messages.
Quote it. If no durable record exists, reconstruct it faithfully and plan to
persist it (step 2's "original ask" block) so it stops being lost. Do **not**
proceed to snapshot open work until you can state the objective the way it was
asked — otherwise the whole handoff risks describing the tickets instead of the
goal.

### 2. Snapshot the open work (verify against live state, not memory)

- **Merged this session:** `ghapp pr list --state merged` filtered to the PRs you
  actually opened this session (by branch/title — do NOT use `--author @me`, it
  returns every PR the account opened across all sessions and will pollute the doc).
- **Open tracking issues** you filed: `ghapp issue view <N>` for each.
- **Open PRs still in flight from this work** (again, filter to *this* session's
  branches — the account has many unrelated open PRs).
- **Worktrees / branches** not yet cleaned: `git worktree list`.
- **Tags:** confirm each merge tagged; note any gap (see the `ci` skill's
  auto-release-gap note and memory `auto-release-drops-pending-run-orphan-tag`).
- Use `ghapp` for every GitHub read, never plain `gh`.

### 3. Write the status doc into the planning submodule (on main)

Path: `planning/<YYYY-MM-DD>-<slug>-handoff.md`. Get planning onto main first:

```
cd planning && git fetch -q origin main && git checkout -q main && git pull -q origin main
```

Structure the doc so a cold reader can act:

1. **Header** — date, from/to, one-line status, one-line goal.
2. **The original ask** — the project's north-star objective and full scope in the
   user's own framing, placed prominently at the very top (before the status
   sections), plus one line on what is still owed against it. This is the section
   that stops a mid-project handoff from silently shrinking the goal — write it
   first, and make clear the tracked issues are *how* it's measured, not a
   narrower substitute for it.
3. **What this was / current state** — the honest status of that objective now.
4. **What landed** — a table of merged PRs + tags (verified, not claimed).
5. **What is OPEN** — the tracking issues with their item lists; say which to
   *drive* vs merely *monitor*, and which is highest priority (a correctness bug
   outranks cosmetic refactors).
6. **How to continue** — the working pattern that succeeded (e.g. area-batched
   behavior-preserving PRs; "done" = artifact-on-main + old-dup-gone + test, not
   "CI green"; ship via `shipyard pr`).
7. **New-machine setup** — submodule init, uncommitted external SDKs (VST3/AU
   symlinks, Skia cache), Release build, secrets location, `ghapp`/SSH.
8. **Gotchas hit** — the specific traps this work paid for, so they aren't
   relearned (link the relevant memories/skills).
9. **Suggested skills** — the `.agents/skills/*` the next session should invoke
   for this work, each with one line on *why*. Point at the skill, don't restate
   it. Keep this agent-neutral: these skills are read by both Claude Code and
   Codex, so name skills, not agent features.
10. **Where the work lives (worktrees / branches)** — state plainly whether
   anything is stranded in a worktree or all landed on `main`; if worktrees were
   removed, say so ("do not look for them"); tell the reader to start from a fresh
   worktree off `origin/main`, not a reused one. This is the item that most often
   confuses a cold session on a new machine.
11. **Definition of done** — the terminal state for the next session.

Optionally add a short **provenance** note *only as theoretical last-resort
backup*: the originating session-transcript path and host, explicitly framed
"prefer the committed artifacts; you should not need this." Do **not** encourage
routine use of the transcript — committed code + issues are the source of truth,
and a transcript path is machine-local and non-portable. Omit it entirely if the
committed artifacts fully cover the handoff.

Always save the doc to the **pulp-planning** submodule on `main` (durable,
identical on every machine), not a temp directory — a handoff whose whole purpose
is to survive the session must not live in scratch space.

Honesty bar: report merged/tagged/done from **live state**, never from a PR body
or a planning claim — PR prose routinely overclaims. If something is partial, say
so with the missing sub-step.

### 4. Commit + push to pulp-planning main

```
cd planning && git add <the-doc>.md && git commit -m "handoff: <slug> — <one line>" && git push origin main
```

**Do NOT bump the parent repo's planning gitlink** as a side effect. Stage only
the doc inside `planning/`; never `git add` the submodule pointer in the pulp
repo without a `Planning-Bump: reason="..."` trailer (see the planning-gitlink
guard). The doc is reachable on planning `main` regardless of the parent pointer.

### 5. Emit the standing-objective prompt

Give the user a ready-to-paste block: a **simple objective** naming the goal, a
**link to the doc** on planning main
(`https://github.com/danielraffel/pulp-planning/blob/main/<doc>.md`), and the
**open-item issue numbers**. Keep the objective itself to a couple of sentences —
tell the reader the detail is in the doc.

Write it agent-neutral so it drops into whichever session picks it up:
- **Claude Code** — paste it as the opening message of a fresh session. (Some
  Claude Code setups also have a standing-objective command — e.g. `/goal`, or
  `ralph-loop` for a sustained autonomous loop — but neither is a repo asset and
  Codex has no equivalent, so never *depend* on one: the prompt text itself must
  carry the persistence instruction.)
- **Codex CLI** — paste it as the opening prompt, or pass it to
  `codex exec "<prompt>"`. Codex reads `AGENTS.md` (which redirects to
  `CLAUDE.md`), so keep repo conventions in the *doc* it links, not in the prompt.

Because no slash-command or persistent-objective feature is guaranteed on either
side, the prompt **must state its own persistence** — "keep going until the doc's
definition-of-done is met" — and must not name a mechanism only one agent has.

Example shape:

> **Goal:** Finish the remaining <work> per the handoff doc
> `<planning-url>`, and monitor issues #A / #B / #C until each is landed-or-parked.
> The doc has the full item list, the working pattern, new-machine setup, and the
> gotchas — follow it; verify every "done" against merged code, not PR bodies.

### 6. Hand off the open items for tracking

List the open issues/PRs explicitly in the reply (not only in the doc) so the
user can point the next session at them, and note who should *drive* vs *monitor*.

### 7. Retire-safety check (before ending the session or killing any watched process)

The user often wants to end this session — or kill whatever background jobs, watch
loops, or monitored agents it left running — and be **confident nothing under
active watch is dropped.** (What "background work" means varies: a Claude Code
session may hold agent-tracked `run_in_background` jobs / Monitor loops / teammate
agents; a Codex or plain-CLI session more likely left a shell job running in
another terminal. The check is the same either way.) Before you say "safe to
retire," prove every monitored thing is in a **terminal** state:

- Each PR the session was watching is `MERGED` (or explicitly closed).
- Each expected tag exists on the remote (check for auto-release gaps — a merged
  bump with no tag is NOT terminal; gap-fill it first, see the `ci` skill).
- No CI/auto-release run is still pending for any of the session's commits.

State the result plainly in the doc's **§0 / monitoring-status** section and in the
reply: *"everything monitored is terminal; background tasks can be killed with zero
loss; the remaining work is issues, driven by the next session, not polled."* If
something is NOT terminal (a queued release, an un-landed tag, a PR mid-merge),
either finish it or name it as a live item the next session must pick up **before**
anything is killed. A handoff that leaves a pending release unwatched is a dropped
release.

## Notes

- **Don't assume the receiving session carries anything forward.** A same-account
  Claude Code session may have persistent cross-session memory; Codex and any
  fresh-machine or fresh-account session have **zero** implicit carryover beyond
  the repo (`AGENTS.md` → `CLAUDE.md`) and the doc you write. Write the doc as if
  the reader has only the repo and the doc — nothing else.
- This skill produces documentation and a prompt; it does not itself merge or
  close anything.
- Prefer one durable doc on planning main over pasting a wall of status into chat
  — the doc survives the session and is the same on every machine.
- If a prior handoff doc for the same work exists, supersede it (link forward)
  rather than forking a second source of truth.
