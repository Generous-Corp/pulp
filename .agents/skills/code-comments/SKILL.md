---
name: code-comments
description: >-
  How to write source comments, doc comments, and test names/tags that have
  lasting value — and what to never write. Consult before adding or editing
  comments in core/**, examples/**, tools/**, test/**, docs/**, or any shipped
  source. Handles "write a comment", "is this comment ok", "comment hygiene",
  and keeping new code from adding the breadcrumbs the cleanup stream removes.
---

# Writing Durable Comments

A comment earns its place by explaining something the code cannot say for
itself and that will still be true next year: current behavior, an invariant, a
non-obvious "why", an upstream/vendor/host quirk, a security or compat
constraint, a regression- or RT-safety guard. It must not encode *how the code
got here* — that provenance belongs in the commit message, `planning/`,
`docs/reports/`, the changelog, or a commit trailer. This repo will be
open-sourced; every comment is read by strangers with no access to our issue
tracker or session history.

This skill exists so *new* code stops adding the exact provenance the repo-wide
cleanup is removing. The cleanup backlog + classification rules are parked at
`planning/2026-07-02-comment-hygiene-PAUSED.md`.

## Never write these (the "Remove" category)

- **Phase / milestone / slice labels.** No `(Phase 4)`, `Phase N will…`,
  `4f`-style sub-phase labels, `Tier A Slice 7`, `Feature 3`, `plan item 7.2`,
  `item 6.12 follow-up`. `[phaseN]` / `[phase3-large]` Catch2 tags are forbidden.
  Capital-letter+digit plan tags (`C2`, `D6`, `M1`) are the same kind of
  breadcrumb — the lint can't safely match them (they collide with MIDI note
  names and register labels), so it's on you to keep them out of comments.
- **Agent / review breadcrumbs.** No `Codex P1/P2/P0`, `codex review`,
  `[codex-*]`, "per the reviewer", "sub-PR", "slice N of".
- **Process/pipeline provenance.** No `[coverage]`, `[codecov]`, `[requested]`
  selector tags; no "added for coverage".
- **Clean-room / reference-lineage *process* notes.** The clean-room audit
  trail lives in the `Reference-Lineage: cleanroom …` **commit trailer**, never
  in a source comment. Describe the DAW/host behavior, not how you derived it.
- **Bare `#1234` as the explanation.** A PR/issue number is not a reason. If the
  issue explains a still-live invariant, state the invariant; a durable
  `[issue-NNN]` *anchor* on a regression test is fine (note: `#` is reserved in
  Catch2 tags — use `[issue-NNN]`).
- **Speculative future / roadmap.** No "Future v2 …", "will support X later",
  TODO-for-a-someday-feature. (Real example removed: a `Future v2 license-key
  payloads (post-RSA migration)` bullet that was already inaccurate — current
  crypto is AES-256-GCM.)
- **`WIP` / "temporary" / "hack for now".** Either it's the behavior (describe
  it) or it's a tracked task (put it in `planning/`).
- **Stale claims.** A comment describing code that no longer exists is worse
  than none (real example removed: an orphaned `CMAKE_BUILD_TYPE` paragraph in a
  test that no longer checks a build-type field).

## Always safe to write (the "Keep" category)

- Upstream / vendor quirks, with a link or version anchor.
- Security constraints, threat-model notes.
- Compatibility constraints and stated non-goals.
- Regression-guard and RT-safety rationale ("no allocation on the audio
  thread", "denormals flushed here").
- The non-obvious *why* behind a surprising line.

## Rewrite, don't delete, when there's a real invariant

Provenance that encodes a stable fact gets **restated as current behavior**,
stripped of the phase/PR wrapper. Real before → after cases from the cleanup:

| Before (process provenance) | After (durable behavior) |
|---|---|
| `Phase 4d adds feedback` | `feedback needs a previous-block slot` |
| `pulp_build_info.hpp (Tier A Slice 7)` | `Build info` |
| test header `macOS plan item 7.2 / 7.1b` | header describing the *current coverage* |
| MPE header naming a phase | "describes UMP status handling and shared factories" |
| raw-MIDI sysex case naming a PR | "describes aborted-sysex recovery" |

## Test names and tags

- No `#` in Catch2 tags (reserved). Use `[issue-NNN]` for a durable regression
  anchor, and *behavioral* tags for what the test covers: `[rt-safety]`,
  `[parity]`, `[thread]`. Never `[phaseN]`, `[codecov]`, `[coverage]`,
  `[requested]`, `[codex-*]`, "which session shipped it".
- A `TEST_CASE` name states what is verified, not when/why-in-the-project it was
  added.

## Self-check before you commit

Scan your own diff for the breadcrumbs above — cheaper than a cleanup PR later:

```bash
# stale provenance in ADDED lines of the staged/working diff
git diff --cached -U0 | rg '^\+' | \
  rg -n -iE '\b(phase\s?[0-9]|slice [0-9]|tier [a-z] slice|plan item|sub-?pr|codex (p[012]|review)|\[(phase[0-9a-z-]*|codecov|coverage|requested|codex[a-z0-9-]*)\]|future v[0-9]|WIP|clean-?room)\b' \
  && echo "↑ reconsider these — see the code-comments skill" || echo "clean"

# repo-wide evergreen-scope lint (docs/reference/** + skills)
python3 tools/scripts/docs_noise_lint.py --mode=report
```

Enforcement today: `docs_noise_lint.py` covers `docs/reference/**` and
`.agents/skills/**/SKILL.md` (diff-scoped in CI + pre-push). Source-comment
enforcement is the author's responsibility until the lint's source scope lands —
so run the `rg` self-check above on any source diff that adds comments.

## The rule in one line

Write the comment as a present-tense statement of what the code does, why it's
non-obvious, or what external constraint it honors. The narrative of how it was
built goes in the commit and `planning/`.
