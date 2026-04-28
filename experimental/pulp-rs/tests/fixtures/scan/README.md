# `tests/fixtures/scan/` — Phase 6b scan fixtures

Each subdirectory stages a synthetic plug-in file tree that the
`scan_parity_test` integration test points the Rust scanner at. The
fixtures **do not** contain real plug-in binaries — the Phase 6b
scanner is a file-enumeration stub (see `UPSTREAM_SYNC.md`), so the
fake payloads below are opaque byte strings.

| Fixture         | Contents                                                 | Purpose                                                         |
|-----------------|----------------------------------------------------------|-----------------------------------------------------------------|
| `mixed_formats` | CLAP, VST3, AU (.component dir), LV2 bundles             | Exercises the all-format scan + the `--format <name>` filter    |
| `empty`         | empty root dirs                                          | Verifies the "No plugins found." fallback                       |

To avoid shell-expansion surprises during `cargo test`, the fixtures
are planted on-disk here (not programmatically in the test) so a
simple `ls` confirms what's in scope.
