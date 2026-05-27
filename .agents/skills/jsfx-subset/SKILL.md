---
name: jsfx-subset
description: Work in Pulp's bounded JSFX lane using source-only examples, subset validation, and explicit exclusions like no `@gfx`.
requires:
  scripts:
    - tools/scripts/jsfx_subset.py
  tools: []
---

# JSFX Bounded Subset

Use this skill when a task involves Pulp's current JSFX support lane.

## Truthful Position

Pulp supports a **bounded audio-focused JSFX authoring lane**, not full REAPER
parity.

Supported now:

- `.jsfx` source files
- subset validation for `@init`, `@slider`, `@block`, `@sample`
- slider declarations `slider1..slider64`
- source-only examples that stay inside that subset

Not supported by this skill:

- `@gfx`
- file I/O
- silent compatibility assumptions with REAPER
- a shipped JSFX runtime/engine in Pulp core

## Core Files

- `docs/guides/jsfx.md`
- `tools/scripts/jsfx_subset.py`
- `examples/jsfx-gain/`
- `examples/jsfx-tremolo/`
- `examples/jsfx-delay/`

## Recommended Workflow

### 1. Validate the file first

```bash
python3 tools/scripts/jsfx_subset.py doctor \
  --file examples/jsfx-gain/PulpJsfxGain.jsfx
```

Use `--json` when another tool or script needs structured output.

### 2. Stay inside the bounded subset

Allowed sections today:

- `@init`
- `@slider`
- `@block`
- `@sample`

If a file uses `@gfx` or another unsupported section, treat that as an explicit
out-of-scope failure, not a bug in the validator.

### 3. Keep docs truthful

When updating this lane, keep these in sync:

- `docs/guides/jsfx.md`
- `docs/reference/capabilities.md`
- `docs/guides/examples.md`
- `planning/v3-phase14-gap-closure-status.md`

## What To Check

For the current JSFX lane, verify:

- `desc:` exists
- slider declarations are sane and unique
- unsupported sections fail clearly
- docs do not imply full REAPER JSFX compatibility or a shipped runtime

## Gotchas

- **Section directives can carry trailing arguments.** REAPER documents
  `@gfx WIDTH HEIGHT` as the standard form for declaring the preferred
  plugin window size. Any future regex change to `SECTION_PATTERN` in
  `tools/scripts/jsfx_subset.py` must keep matching the `@name <args>`
  shape — anchoring to end-of-line silently lets `@gfx 200 200` slip past
  the bounded-subset check (the exact bug the 2026-05-26 follow-up fixed).
  Regression coverage lives in
  `tools/scripts/test_jsfx_subset.py::test_doctor_rejects_gfx_section_with_size_directive`.
