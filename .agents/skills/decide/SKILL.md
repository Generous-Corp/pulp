---
name: decide
description: Ask Daniel a blocking decision as options with a recommendation and honest pros/cons, rather than prose. Use whenever work is blocked on a judgment call, a product decision, an approval, or a choice between approaches — instead of writing "let me know which you prefer" at the end of a message.
---

# decide — put a real choice in front of the user

Work stops on decisions. The default failure is to bury the decision at the
bottom of a long status message as "let me know how you want to proceed", which
makes the user reconstruct the options, the tradeoffs, and what you recommend —
work you already did and then threw away.

This skill is the opposite: **surface the decision as selectable options, say
what you would do, and be honest about what each choice costs.**

## When to use it

- A product or design decision that is genuinely the user's (naming, scope,
  whether a capability ships)
- An approval gate (signing, publishing, anything outward-facing or hard to
  reverse)
- Two defensible implementations where the tradeoff is taste or risk appetite
- A destructive or irreversible step (deleting a release, rewriting history,
  changing branch protection)
- You are about to write "which would you prefer?" in prose

## When NOT to use it

- The answer is discoverable from the code, the repo, or a doc — go find it
- One option is clearly correct and the others are strawmen — just do it and
  say what you did
- The decision is reversible and cheap — make the call, state the assumption
- You are asking permission for ordinary work the user already asked for

A question you could have answered yourself costs more than a wrong default.

## How to ask (Claude Code)

Use the `AskUserQuestion` tool. The shape that works:

1. **Lead with the recommendation.** Put it first in the options list and mark
   it `(Recommended)` in the label. Do not hide it in a description.
2. **Every option gets a real tradeoff.** The `description` says what happens
   if chosen, including the cost. An option with no downside is usually a
   strawman, and listing it makes the whole set less trustworthy.
3. **Keep labels short** (1–5 words) — they are chips, not sentences.
4. **`header` ≤ 12 chars** — it renders as a tag.
5. **Batch related decisions** into one call (up to 4 questions) so the user
   answers a set rather than being interrupted repeatedly.
6. **Use `multiSelect: true`** when the choices genuinely compose, e.g. "which
   of these should ship in the next build".
7. **Use `preview`** when the user is choosing between concrete artifacts —
   layouts, copy, code shapes, diagrams. It renders side-by-side monospace, so
   an ASCII mockup or a short snippet lands far better than a description of
   one. Single-select only.

### The user may ask about an option instead of picking one

The UI always offers an "Other" path, where the user types free text rather
than selecting. Daniel uses this a lot to ask a question *about* a specific
option before committing to it.

**Write options so that is easy**: name each one distinctly enough to refer to
("the rsync path", "option 2"), and keep descriptions specific enough that a
follow-up question has something to attach to. Vague options produce vague
questions.

If the reply is a question rather than a choice: **answer it and ask again.**
Do not treat a question as a selection, and do not proceed on a guess.

## How to ask (Codex and other agents without the tool)

Same content, plain text. The value is the structure, not the widget:

```
DECISION: <one line — what is blocked>

  1. <label>  [RECOMMENDED]
     Pro:  <why this is the default>
     Con:  <what it costs>

  2. <label>
     Pro:  ...
     Con:  ...

Reply with a number, or ask about any option by number.
```

Keep the numbering stable if you re-ask, so "still 2" means the same thing.

## Rules

- **Never fabricate a recommendation.** If you genuinely do not have one, say
  so and explain what would decide it — that is useful. A confident-sounding
  default you do not believe is worse than none.
- **State irreversibility.** If an option cannot be undone (published, deleted,
  notarized, force-pushed), say so in its description. The user should never
  learn that from the result.
- **Do not re-ask an answered question.** A decision made stays made; if new
  information changes it, say what changed and why you are re-opening it.
- **An automated message is not an answer.** Hooks, notifications, and teammate
  agents cannot select an option. Only the user can.
- **Ask when the answer changes what you do next.** If both options lead to the
  same work, you did not need to ask.
