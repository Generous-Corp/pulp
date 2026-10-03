# Dependency Tooling

This directory is the machine-readable dependency inventory and update-validation lane for Pulp.

## Files

- `manifest.json` — source of truth for tracked third-party dependencies, pins, licenses, and documentation requirements
- `audit.py` — audits `DEPENDENCIES.md`, `NOTICE.md`, and `docs/reference/licensing.md` coverage against `manifest.json`; optionally checks upstream refs/tags
- `validate_hosts.py` — runs outer-loop validation locally and on optional SSH targets

The audit script covers all four attribution surfaces (`manifest.json`,
`DEPENDENCIES.md`, `NOTICE.md`, `docs/reference/licensing.md`) so drift
between them is caught at PR time rather than after release.

## Usage

```bash
# Audit docs/notice coverage
python3 tools/deps/audit.py --strict

# Audit coverage plus upstream drift
python3 tools/deps/audit.py --check-upstream --format markdown

# Run local outer-loop validation plus any configured SSH hosts
python3 tools/deps/validate_hosts.py
```

`--verify-licenses` reads the checked-out dependency trees. It also holds every
manifest entry marked `"offline_fetch": {"cache_name": "<FetchContent name>"}`
to an offline-fetch contract: its CMake files fetch only through
`FetchContent_Declare`, never a raw `file(DOWNLOAD)`. A changed-surface bounded
run configures its protected base with `FETCHCONTENT_FULLY_DISCONNECTED=ON`
(`run_changed_surface_tests.base_projection`), which governs FetchContent and
nothing else, so a raw download in such a dependency could reach the network
mid-plan. When you bump a marked dependency, the audit fails with the file and
line if the new pin downloads outside FetchContent. A tree that is not checked
out is reported as unverified, never as a pass. On a configured macOS build the
`deps-offline-fetch-contract` ctest runs
`audit.py --offline-fetch-only --require-offline-trees --build-dir <build>`:
it reads the tree the build configured with (`<cache_name>_SOURCE_DIR` in the
CMake cache) and fails if that tree is absent, so the gate and the m3 lane can
never report "no raw downloads" without having read one.

## Local SSH Host Config

Create `tools/deps/hosts.local.json` for your machine-specific validators. This file is gitignored.

Example:

```json
{
  "unix_targets": [
    { "host": "ubuntu", "path": "/home/daniel/Code/pulp" }
  ],
  "windows_targets": [
    { "host": "win2", "path": "C:\\\\Users\\\\danielraffel\\\\Code\\\\pulp" }
  ]
}
```

Remote validation is intended to be git-based:

- push the branch to `origin`
- point each target at a real `git clone` / worktree of the repo
- let the validator `git fetch` / `git checkout` / `git pull` before running the clean build
- ensure required build tools are installed on that machine

If a remote checkout does not have an `origin` remote configured, the validator currently falls back to validating the current checkout (or a matching local branch if present) so personal machines still produce a useful result. That fallback is a convenience, not the preferred setup. If the configured path is not a real git checkout, validation fails clearly.
