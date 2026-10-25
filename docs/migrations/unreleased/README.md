# Unreleased migration notes

Write the migration note for a change here, not as `../vX.Y.Z.md`. A pull
request cannot know which version it ships in: version-at-land assigns the
version after the change lands, in merge order, so a note keyed to a guessed
version goes stale whenever another release lands first.

Name the file after the change (`compressor-opt-in.md`, `knob-api-rename.md`).
The frontmatter takes only `breaking` and `summary`:

```markdown
---
breaking = true
summary = "Compressor auto-makeup is now opt-in"
---

## Breaking changes
- ...

## Migration steps
- ...
```

Do not set `version` or `applies_if`; the build refuses a note that does. The
bump commit that assigns the next SDK/CLI version moves every note in this
directory into `../v<version>.md`, writing both fields
(`tools/scripts/fold_migration_notes.py`, called by
`tools/scripts/version_at_land.py`). Until then the note is validated when
`pulp-cli` builds but is not shown by `pulp upgrade --notes`, because no
released CLI can be upgraded to it yet.
