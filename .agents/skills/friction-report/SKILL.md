---
name: friction-report
description: Turn a moment of friction — a conflicting PR, a wedged runner, a mysterious red check, a repeated manual chore — into a durable, actionable report. Sizes up the scenario, captures the evidence, separates one-off from SYSTEMIC, proposes a fix, and routes it (fix now, or hand off). Reports are LIVING documents that the fixing agent updates with the outcome. Invoke whenever something feels like it shouldn't have been this hard, or when the same annoyance shows up twice.
---

# Friction report

**The point:** most friction is paid twice — once when it happens, and again every time it
happens to someone else. This skill pays it *once* by writing it down in a form the next
agent can act on.

**The trap this exists to avoid:** an agent fixes a conflicting PR, feels productive, and
never asks *why the PR conflicted*. The rebase takes ten minutes and the root cause survives
to bite thirty more PRs. **A fix without a report is a rerun waiting to happen.**

---

## When to invoke

- A PR is conflicting, red, or stuck, and it is not obvious why.
- You just did something manual that you suspect you will do again.
- The same class of failure has appeared **twice**. (Twice is a pattern. Once is weather.)
- A gate, hook, or workflow blocked something it should not have — or waved through
  something it should not have.
- Daniel says a version of *"why does this keep happening?"*

**Do NOT invoke** for a genuine one-off (a typo, a flaky network call). The bar is: *would
this bite someone else?*

---

## ⛔ STEP ZERO — CONSTRAINT DISCOVERY (mandatory, no exceptions)

**This step exists because the skill failed without it.** Its first real use produced a
confident, well-evidenced, **actively dangerous** proposal — it identified a gate as a bug and
recommended removing it. But the repo *documented* why that gate existed: it was added after a
release silently failed to ship, and removing it would have reintroduced exactly that failure.
The evidence was solid; the proposal was still wrong, because it never asked what the gate was
protecting.

> **Most "obviously wrong" designs are scar tissue. Find the scar before you cut.**

**Do all four. Write the answers into the report. A proposal without this section is invalid.**

### 0.1 — What is this repo's toolchain? (DETECT, never assume)

Do not hardcode. Daniel uses Shipyard and tartci often, but **not everywhere**, and other
repos differ. Detect:

```sh
ls .shipyard/config.toml            # Shipyard?  which gates/targets?
ls .tartci* tools/tartci*           # tartci?    self-hosted runners?
ls .github/workflows/               # plain GitHub Actions? which are REQUIRED?
ghapp api repos/{owner}/{repo}/branches/main/protection -q '.required_status_checks.contexts'
ghapp api repos/{owner}/{repo} -q '.allow_squash_merge, .allow_merge_commit, .allow_rebase_merge'
git log --oneline -5 origin/main    # squash? merge commits? who pushes to main?
```

**A fix that is correct for a plain-GHA repo can be wrong for a Shipyard repo, and vice
versa.** Say in the report which toolchain you actually found.

### 0.2 — Does the repo DOCUMENT a reason for the thing you are calling a bug?

**This is the step that was skipped.** Search, in this order:

```sh
rg -i "why|rationale|deliberate|on purpose|do not|never" docs/guides/ CLAUDE.md
git log -S"<the mechanism>" --oneline           # the commit that introduced it
git log --format='%B' -1 <that commit>          # and WHY, in its message
ls planning/friction/ planning/decisions/       # has this been decided before?
```

If a comment says **"DO NOT FIX IT"** or a doc explains a design, **that is the answer** —
not an obstacle to route around.

### 0.3 — Find the MOTIVATING INCIDENT

Gates, guards, and awkward conventions almost always exist because something broke.

**Ask: what did this prevent?** If the answer is *"I don't know,"* you are not ready to
propose removing it. Search for the issue/PR number in the docs, the code comments, and the
git history.

### 0.4 — Would your fix REINTRODUCE the bug the current design prevents?

State it explicitly, in the report:

    Current design prevents: <the failure>   (evidence: <incident>)
    My proposal preserves that guarantee by: <how>
    ...or it does NOT, and here is the trade: <what you'd be giving up>

**If you cannot answer this, you have a hypothesis, not a proposal.**

### 0.5 — VERIFY THE MECHANISM: can the fix actually catch the failure?

**This step exists because a report failed without it.** A git-state report proposed a
*pre-push hook* to catch a silent no-op push — but a no-op push pushes nothing, so the hook
**never fires.** The fix was proposed for a failure its own mechanism was structurally blind
to. It read as confident and specific and it was wrong, and it was only caught because a human
pushed back.

> **Before you write a fix down, prove its mechanism can detect the failure it targets.**
> A check cannot catch what does not run. A pre-push hook cannot see a commit-time problem. A
> level-triggered alarm cannot see a transition. Assume nothing about what a mechanism can
> observe — test it.

**Map each failure to the ONE mechanism that actually catches it**, and say why the others do
not:

    | failure | mechanism that WORKS | why not the alternatives |
    |---------|----------------------|--------------------------|
    | ...     | ...                  | ...                      |

If a fix bundles several failures under one mechanism ("a preflight will catch all of these"),
that is the smell. Split it. Usually the failures need *different* mechanisms — a wrapper for
one, a hook for another, a habit for a third — and some may have **no clean mechanism at all**,
which you must say plainly rather than paper over.

*(This is the same discipline as the personal `confirm-the-failure` skill, applied to the
FIX rather than to a measurement: prove the instrument can report the thing before you trust
it. If that skill is available in your environment, use it here.)*

---

## Confidence — state it, and say what would raise it

Every proposal ends with an honest line:

    Confidence: HIGH | MEDIUM | LOW
    Why: <what I verified>
    What would raise it: <the check I did not run>

**"I verified the diagnosis but not the fix" is a legitimate and useful thing to publish.**
Overstating confidence is how a report becomes worse than no report.

---

## The three questions, in order

**1. What is stuck, exactly?**
Not "CI is red" — *which* check, on *which* commit, failing *how*. Get the evidence before
the theory. Use `ghapp`, never plain `gh`.

**2. Is this a ONE-OFF or is it SYSTEMIC?**
This is the whole value of the skill, and it is the question people skip.

> **Ask: could this have happened to any other PR this week?**
> If yes, it is systemic, and the fix is not "rebase this branch."

Signals of systemic:
- The conflict is in a file that *every* PR touches (version files, a shared SKILL.md, a
  generated manifest, a lockfile).
- The failure is produced by our own gate, hook, or convention.
- It has happened before (search the friction reports and the decision log first).

**3. What is the smallest change that makes this class impossible?**
Not "be more careful." Not "remember to rebase." A **mechanism**.

---

## Output: the report

Write to `planning/friction/YYYY-MM-DD-<slug>.md` (private submodule). Use this shape:

```markdown
# <one-line title: what got stuck>

**Status:** OPEN | IN PROGRESS | FIXED | WONTFIX | SUPERSEDED
**Filed:** <date> by <agent>@<host>
**Owner:** <unassigned | agent | Daniel>
**Class:** one-off | SYSTEMIC
**Recurrence:** first time | seen N times (link prior reports)

## What happened
Plain language. What was being attempted, what blocked it, how long it cost.

## Evidence
The commands and their output. Not a narrative — the receipts.

## Root cause
Why it ACTUALLY happened. If the answer is "someone forgot," dig again:
a mechanism that depends on remembering is the root cause.

## Why it will recur
The specific reason this is not a one-off. Name the other PRs/agents/days it would hit.

## The fix now (unblock)
The tactical thing that gets this specific instance moving. Usually boring.

## The fix forever (prevent)
The mechanism that makes this class impossible. Ranked if there are options,
with the cost of each. Say plainly if the good fix is blocked on something else.

## Routing
- [ ] Fix now, by me
- [ ] Hand off — needs <what>: <why I am not doing it>

## Resolution   ← the fixing agent MUST fill this in
**Outcome:** <what was actually done>
**Landed in:** <PR / commit>
**Did it work?:** <verified how>
**Still open:** <what was NOT fixed, and why>
```

---

## Routing: fix now, or hand off?

**Fix now** when the fix is bounded, reversible, and you have the context loaded.

**Hand off** when any of these are true — and **say which one**:
- It needs a decision that is Daniel's (cost, policy, an irreversible action).
- It touches a system you would be guessing about.
- The real fix is blocked on something not yet built. *(Then the report IS the deliverable —
  it becomes the spec.)*
- You are near a quota wall, or the fix is bigger than the session you have left.

> **Handing off is not failure.** A precise report that another agent can execute is worth
> more than a rushed fix that misses the root cause.

---

## The living-document rule (non-negotiable)

**Any agent that picks up a friction report MUST update its `Resolution` section** — even
if the answer is *"tried, did not work, here is why."*

An unresolved report with an honest Resolution is useful.
A silently abandoned report is worse than no report: **it looks like the problem was fixed.**

> **Absence of a resolution is never evidence of resolution.**

Before filing a new report, **grep `planning/friction/` for the same symptom.** If it exists,
bump its `Recurrence` and add to it rather than filing a duplicate. Three recurrences on one
report is a promotion signal — that class needs a real fix, now.

---

## Anti-patterns

| Don't | Do |
|---|---|
| "Rebased and re-pushed. Fixed!" | Ask *why it conflicted*, then write it down. |
| A report with no evidence | Paste the commands and their output. |
| "We should be more careful" | Name a **mechanism**. Carefulness does not scale to 35 PRs/day. |
| Filing a duplicate | Grep first; bump `Recurrence`. |
| Fixing it and closing without a Resolution | The Resolution is the *point*. |
| Blaming an agent | Blame the mechanism that let it happen. |

---

## Roadmap — from filing to acting (do NOT skip levels)

Today the skill files a report to `planning/friction/` (private, synced to every machine).
That is **Level 0**, and it is deliberately the whole product for now. Reports pile up; a
human reads them at their own pace. The value is passive: after a handful you can see whether
the reports are *accurate and root-cause-real*, or noise.

**Graduate only when the current level has earned it. Each level is a trust milestone, not a
calendar date.**

| Level | Behaviour | Graduate when… | Never |
|---|---|---|---|
| **0 — now** | Files to `planning/friction/`. Human reads. | ~5–10 reports are accurate, name real root causes, and pass Step Zero (no dangerous proposals). | — |
| **1 — triage** | Human triages each report: fix-now / hand-off / wontfix. Report is the spec. | Level-0 reports consistently earn "yes, act on this." | — |
| **2 — assisted act** | An agent executes a report **the human greenlit**, then fills in `Resolution`. | Hand-offs land correctly without the human re-explaining. | Act on anything a human did not greenlit. |
| **3 — bounded autonomy** | Agent auto-acts on **pre-approved low-risk classes only** (e.g. rebase-a-stale-PR), logs to `#activity`, escalates the rest. | Level-2 has a track record on that specific class. | Destructive or irreversible actions — always stop at a human. |

### Design constraints for the automated levels (decided, not open)

- **Private by default.** Reports stay in `planning/friction/` (the private submodule). A
  public GitHub issue, if ever wanted, is a **sanitized pointer** ("CI friction filed — see
  internal tracker"), never the raw report. Reports routinely contain host names, account
  emails, internal incident numbers, and command output. **Nothing sensitive goes public.**
- **Assignment is opt-in per class.** Auto-assigning an agent to a report is a Level-2/3
  feature, and only for classes the human has explicitly marked auto-actionable. Everything
  else waits for triage.
- **The living-document rule is what makes autonomy auditable.** An acting agent MUST update
  `Resolution`. Without it, an autonomous fix is indistinguishable from an autonomous
  mistake. **This is the invariant the higher levels rest on — build it into every actuator,
  not as an afterthought.**
- **Recurrence is the promotion signal.** A report that recurs N times is the system telling
  you that class deserves a real (Level 2/3) fix. Wire the recurrence counter to surface
  those, rather than deciding by hand which classes to automate.
- **Verify the instrument before acting on it.** Before an agent acts on a report's finding —
  especially to change code, revert, or block a merge — it must confirm the finding is real
  (run the check against a known-good control). A report is a *claim*, not a verdict. See the
  `confirm-the-failure` discipline.

### The anti-goal (restate at every level)

This never becomes a bot that files noise or auto-closes things to look productive. The
metric is **not** reports filed or issues closed. It is **classes of friction that stopped
recurring** because a report led to a real fix. A folder full of OPEN reports that no one
acted on is a *failure of triage*, not a success of filing.
