# Captured generated-output corpus

This fixture is a small, deterministic source capture from the checked-in
Claude Design conformance fixture at
`test/fixtures/imports/claude/2024.10/example.html`. The exact HTML input is
copied under `conformance/claude-design.html`; its `.card`, `.tnum`,
`.btn-primary`, and `.btn-secondary` semantics are represented in named TSX
source. It is checked by the same clean-output lint as the importer acceptance
corpus.

`manifest.json` records every listed corpus byte. Entries with `owned-source` or
`emitted-source` roles are linted; `conformance-fixture` and `generated-vendor`
entries are integrity-checked and explicitly excluded from semantic findings.
