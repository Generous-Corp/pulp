# Pulp sandbox E2E test harness

Isolated end-to-end tests for the Pulp CLI + Claude plugin. Every test
runs in a fresh tempdir with a shadowed `$PATH` and isolated
`$PULP_HOME`; the contamination audit enforces **zero writes** to the
user's real `~/.pulp/` install.

Tracked in [pulp#732](https://github.com/danielraffel/pulp/issues/732).
Sibling harness pattern for Shipyard lives in
[Shipyard#248](https://github.com/danielraffel/Shipyard/issues/248).

## Why this exists

The one-off sandbox we built to validate the Phase 8 Rust CLI swap
caught a silent-exit-0 regression (`pulp ship sign` → "Unknown
command: ship" + exit 0) that would have shipped broken and taken out
every plugin slash-command. Unit tests can't catch that class of bug:
it requires invoking the real installed binary the way users do and
asserting the observable behavior is sane.

This harness codifies that pattern so we keep catching it.

## What it covers

- **Plugin command surface** — every `pulp X` invocation in
  `.claude/commands/*.md` must either run natively or delegate cleanly
  (never silent-exit-0).
- **Cross-binary state continuity** — C++ writes `config.toml` /
  `projects.json`, Rust reads the same values after a sim-swap.
- **`PULP_USE_CPP=1` rollback** — verifies the rollback lever works
  against the real C++ binary, not a stub.
- **`pulp upgrade --check-only`** — JSON schema is well-formed; no
  network hit when `PULP_UPDATE_CHECK_DISABLED=1`.
- **Parity probes** — `doctor --versions --json` schema matches
  between the C++ and Rust binaries.
- **Library-linked delegation** — `ship`, `validate`, `inspect`,
  `host`, `audio`, `import-design`, `export-tokens`, `design-debug`
  route from the Rust binary through the Phase 7 fallthrough wrapper
  to `pulp-cpp`.
- **Doctor platform probe** — both `pulp doctor android` (positional,
  legacy) and `pulp doctor --android` (flag) reach `pulp-cpp`.
- **Contamination audit** — runs automatically at every test
  teardown; any write to `~/.pulp/`, `~/.local/bin`, or
  `~/.cargo/bin` fails the test.

## What it deliberately does NOT cover

| Excluded | Why |
|---|---|
| `pulp upgrade` (install path) | mutates `~/.pulp/bin` and fetches from GitHub |
| `pulp sdk install` | downloads SDK tarballs |
| `pulp cache fetch skia` | downloads assets |
| `pulp tool install` | downloads tool archives |
| `pulp ship sign/notarize/package` | signs via system keychain, calls notarization APIs |
| `pulp pr` | orchestrates GitHub + Shipyard |
| `pulp build`/`create`/`run`/`design`/`inspect`/`validate`/`dev` | GUI, long-lived, or needs project fixtures — covered by other test layers |
| plugin hooks (`docs-reminder.sh`, `cli-plugin-sync.sh`) | advisory only, no CLI state |

These live in `SURFACE_SKIPS` in `test_swap.py` — each skip carries a
one-line reason. When a new command lands, the default is "add an
entry here and file a follow-up to write a real test."

## Running

```bash
# from the repo root
pytest tools/sandbox-e2e/

# or just the fast subset
pytest tools/sandbox-e2e/ -m "plugin_surface or rollback or parity"

# pin specific binaries (useful in CI and when iterating on a branch)
PULP_CPP_BINARY_FOR_TEST=/path/to/pulp-cpp \
PULP_RS_BINARY_FOR_TEST=/path/to/pulp \
    pytest tools/sandbox-e2e/
```

## Binary discovery

`conftest.py` resolves the binaries in this order:

1. `PULP_CPP_BINARY_FOR_TEST` / `PULP_RS_BINARY_FOR_TEST` env
   overrides
2. Build artifacts:
   - C++: `build/tools/cli/pulp-cpp` (`build/tools/cli/pulp` on old branches)
   - Rust: `experimental/pulp-rs/target/release/pulp`
3. Fallback for C++: `~/.pulp/bin/pulp-cpp` (`~/.pulp/bin/pulp` on old
   branches), copied into the sandbox — never invoked in place
4. Sibling-worktree fallback for Rust (looks under `../pulp*`)

If neither is found, the relevant tests `pytest.skip(...)` with a
clear message. The harness never runs against "whatever is on PATH."

## Contamination audit

Every `sandbox` fixture records a tempfile mtime at setup. At
teardown, `Sandbox.assert_no_contamination()` walks
`~/.pulp/`, `~/.local/bin/`, and `~/.cargo/bin/` and fails if any
file has a strictly-greater mtime. If a test fails here, the
write path that leaked is in the failure output — fix the code, not
the audit.

Protected paths are in `PROTECTED_PATHS` in `pulp_sandbox.py`.
Extensible via `audit_contamination(extra_roots=(...))` for tests
that want narrower checks.

## Adding a new scenario

1. Decide which `@pytest.mark` applies: `plugin_surface`,
   `cross_binary`, `rollback`, `upgrade`, `parity`, `library_linked`.
   If none fit, add a new mark + document it here.
2. Add a function in `test_swap.py`:

   ```python
   @pytest.mark.<mark>
   def test_<what_it_proves>(sandbox: Sandbox, cpp_binary: Path) -> None:
       sandbox.stage_binary(cpp_binary, as_name="pulp")
       result = sandbox.run(["your", "command"])
       assert ...
   ```
3. If the scenario needs a stub binary / a fixture project / canned
   JSON, drop it under `fixtures/` and request it via a session-scoped
   pytest fixture in `conftest.py`.
4. If the scenario touches a new state path the audit doesn't cover
   yet, extend `PROTECTED_PATHS` — err on the side of overbroad.

## Regression-test contract

This harness exists because of one specific bug
(`4ba25715 experimental/pulp-rs: Phase 8 gap — unknown subcommand
falls through to pulp-cpp`). The
`test_library_linked_command_falls_through_to_pulp_cpp` test is the
dedicated regression. If you revert `4ba25715` locally, that test
MUST fail. This is checked in the
`.github/workflows/sandbox-e2e.yml` workflow as a sanity probe on
release tags.

## Layout

```
tools/sandbox-e2e/
├── README.md                  # this file
├── pulp_sandbox.py            # Sandbox class, binary staging, contamination audit
├── conftest.py                # pytest fixtures: binaries, sandbox, commands dir
├── fixtures/
│   └── stub_pulp_cpp.sh       # deterministic stub that tags stdout + exits 42
└── test_swap.py               # the 8 scenarios from pulp#732
```

## Dependencies

Just `pytest` (Python 3.10+ for the type hints). The harness uses
only stdlib otherwise so it runs in any CI image that has python3.

## CI integration

See `.github/workflows/sandbox-e2e.yml`. Runs on macOS arm64 +
ubuntu-latest on every PR that touches:

- `tools/cli/**`
- `experimental/pulp-rs/**`
- `.claude/commands/**`
- `.claude-plugin/**`
- `tools/sandbox-e2e/**` (the harness itself)

Required status check for merge. Also a pre-release gate so a broken
harness never ships a broken release.
