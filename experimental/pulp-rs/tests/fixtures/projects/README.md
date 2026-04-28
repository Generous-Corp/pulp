# Phase 4 fixtures — `pulp-rs projects list`

Each directory is a distinct input state for the registry file at
`$PULP_HOME/projects.json`:

| Fixture                | Input                                | Exercises                              |
|------------------------|--------------------------------------|----------------------------------------|
| `empty_registry/`      | (no projects.json planted)           | Empty-registry message path.           |
| `single_project/`      | One well-formed entry.               | Singular noun, existing path.          |
| `multiple_with_stale/` | Four entries, two missing on disk.   | Plural noun + stale-entry `(missing)`. |
| `malformed_registry/`  | Forward-compat fields + missing.     | Unknown field tolerance.               |

For each fixture:

- `projects.json` (where present) is the input planted at
  `$PULP_HOME/projects.json` before running the CLI.
- `expected_human.txt` was captured from the C++ `pulp projects list`
  binary and then normalised: the `Registry:` line's absolute path is
  replaced with the placeholder `<REGISTRY>`. Test drivers perform the
  same substitution on the Rust CLI's output before diffing.
- `expected.json` is the shape the Rust `pulp-rs projects list --json`
  lane emits. C++ has no `--json` flag for this command today; the
  shape is documented in `src/cmd/projects.rs` and these files pin it.

The Phase 4 parity test creates `/tmp/pulp-rs-fixture/{alpha-plugin,…}`
at test-setup time so that the `missing_on_disk` field behaves
predictably.
