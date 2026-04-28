# pulp-rs test fixtures

Each subdirectory is a mini standalone Pulp project used to drive the
parity test in `tests/parity_test.rs`. The fixture layout is the
minimum needed to exercise `pulp doctor --versions --json`:

- `pulp.toml` — triggers standalone-project detection
- `.claude-plugin/plugin.json` — when present, exercises the plugin
  skew rules
- `expected.json` (optional) — golden output. When present, the parity
  test compares the Rust output to the saved expected JSON directly.
  When absent, it attempts to shell out to the C++ CLI (if
  `PULP_CLI_PATH` is set) and compare against a freshly-captured
  reference.

See `tests/parity_test.rs` for the exact comparison rules.

## Fixtures

| Fixture | Scenario |
|---|---|
| `ok_plain/` | sdk_version == CLI, no skew. Expect one Info finding. |
| `sdk_ahead/` | sdk_version > CLI. Expect a Warn on rule 2a. |
| `cli_min_ahead/` | cli_min_version > CLI. Expect Warn on rule 1 + rule 2a. |
| `plugin_newer/` | plugin.json `min_cli_version` > CLI. Expect Warn on rule 1b. |
| `registered_projects/` | a project root plus a planted `~/.pulp/projects.json` (parity test plants it via PULP_HOME). |

## CLI version used in fixtures

The parity test sets `PULP_RS_CLI_VERSION=0.38.0` so findings are
deterministic. Fixtures are designed around that reference value.
