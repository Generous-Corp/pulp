# Docs Maintenance

How documentation stays consistent with the codebase across tools, branches, and contributors.

## Enforcement Layers

Pulp uses three layers to keep docs aligned with code:

### 1. CI Check (universal gate)

The `docs-check.yml` GitHub Actions workflow runs on every push and PR to `main` and `develop`. It executes `tools/check-docs.sh` which validates:

- Every `.md` file in `docs/` is indexed in `docs/status/docs-index.yaml`
- Every path referenced in YAML manifests resolves to a real file
- All `status:` values use the allowed vocabulary
- Module dependencies in `modules.yaml` match CMake `target_link_libraries`
- Format adapters claimed in `support-matrix.yaml` have real source files
- Subsystem directories listed in `modules.yaml` exist

If any check fails, the CI job fails and the PR cannot merge.

### 2. Claude Code Hook (agent nudge)

A `PostToolUse` hook in `.claude/settings.json` fires after file edits. When an agent modifies files in `core/`, `examples/`, or `tools/cli/`, it prints a reminder to update docs and manifests. This is a soft nudge, not a blocker.

### 3. AGENTS.md (multi-tool contract)

The repo-root `AGENTS.md` file is read by both Claude Code and Codex CLI. It contains the docs maintenance rule: when you modify source files that affect public behavior, update the relevant YAML manifests and Markdown docs.

## Local Validation

Run the docs consistency check locally at any time:

```bash
# Via the CLI
./build/tools/cli/pulp docs check

# Directly
tools/check-docs.sh
```

Both do the same thing: run all manifest/link/vocabulary/dependency checks and report errors and warnings.

## What Triggers a Docs Update

| Change | Manifests to update | Docs to update |
|--------|-------------------|----------------|
| New CLI command | `cli-commands.yaml` | `reference/cli.md` |
| New CMake function | `cmake-functions.yaml` | `reference/cmake.md` |
| Module dependency change | `modules.yaml` | `reference/modules.md` |
| Format support change | `support-matrix.yaml` | `reference/capabilities.md` |
| Platform support change | `support-matrix.yaml` | `reference/capabilities.md` |
| New example | `docs-index.yaml` | `docs/examples/<name>.md`, `docs/examples/index.md` |
| New subsystem | `modules.yaml`, `support-matrix.yaml` | `reference/modules.md` |
| Style rule change | `style-rules.yaml` | `policies/code-style.md` |

## Branch Model

Docs version with the branch they live on:

- `main` docs describe what is stable and released
- `develop` docs describe what is in development
- When `develop` merges to `main`, docs merge too

All CI workflows (build, test, validate, sanitizers, docs-check) trigger on both `main` and `develop`.

This means you can update docs on `develop` as you build features, and they land on `main` cleanly when the branch merges.

## Adding a New Doc

1. Create the `.md` file in the appropriate `docs/` subdirectory
2. Add an entry to `docs/status/docs-index.yaml` with slug, path, kind, and summary
3. Run `tools/check-docs.sh` to verify
4. If the doc covers a capability, module, or example, update the relevant manifests too
