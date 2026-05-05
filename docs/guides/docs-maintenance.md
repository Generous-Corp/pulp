# Docs Maintenance

How documentation stays consistent with the codebase across tools, branches, and contributors.

## Enforcement Layers

Pulp uses three layers to keep docs aligned with code:

### 1. CI Check (local truth + manual cloud companion)

The authoritative docs consistency check is `tools/check-docs.sh`, which you can
run locally or through the `docs-check.yml` workflow. The current GitHub Actions
workflow is `workflow_dispatch`-only; it does not run automatically on every
push or PR in the current repo state.

`tools/check-docs.sh` validates:

- Every `.md` file in `docs/` is indexed in `docs/status/docs-index.yaml`
- Every path referenced in YAML manifests resolves to a real file
- All `status:` values use the allowed vocabulary
- Module dependencies in `modules.yaml` match CMake `target_link_libraries`
- Format adapters claimed in `support-matrix.yaml` have real source files
- Subsystem directories listed in `modules.yaml` exist

If any check fails, the local docs check or manually dispatched workflow fails.

### 2. Claude Code Hook (agent nudge)

A `PostToolUse` hook in `.claude/settings.json` fires after file edits. When an agent modifies files in `core/`, `examples/`, or `tools/cli/`, it prints a reminder to update docs and manifests. This is a soft nudge, not a blocker.

### 3. AGENTS.md (multi-tool contract)

The repo-root `AGENTS.md` file is read by both Claude Code and Codex CLI. It contains the docs maintenance rule: when you modify source files that affect public behavior, update the relevant YAML manifests and Markdown docs.

## Local Validation

Run the docs consistency check locally at any time:

```bash
# Via the CLI
./build/pulp docs check

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

Not every workflow is always-on in the current repo state. Some workflows remain
manual `workflow_dispatch` entry points while the repo stays local-first for CI.

That means branch truth should be documented explicitly instead of assuming an
always-on cloud gate for every workflow.

## README Accuracy

The CI check also validates that README.md's claimed test count matches the actual `ctest` count in the build directory. This catches drift like "53 tests" persisting while the repo has 270.

## Docs Site

The docs site at `generouscorp.com/pulp/` is generated from the same `docs/` files:

- **Build locally**: `pulp docs build-site` (delegates to `mkdocs build`) or `mkdocs build --site-dir build/site` directly
- **Hot-reload dev server**: `mkdocs serve` watches `docs/` and refreshes on save
- **Deploy**: the `docs-deploy.yml` GitHub Actions workflow builds and deploys on push to `main`
- **Source**: GitHub Pages configured with "GitHub Actions" as the source
- **Base URL**: `/pulp/` (served under the user-level custom domain)

The site is rendered by [MkDocs Material](https://squidfunk.github.io/mkdocs-material/) (see `mkdocs.yml`). Install the docs deps with `pip install -r requirements-docs.txt`. Pre-build drift checks (`docs_generate.py check` and `check-docs-consistency.py`) run automatically via `tools/mkdocs_hooks.py`.

## Adding a New Doc

1. Create the `.md` file in the appropriate `docs/` subdirectory
2. Add an entry to `docs/status/docs-index.yaml` with slug, path, kind, and summary
3. Run `tools/check-docs.sh` to verify
4. If the doc covers a capability, module, or example, update the relevant manifests too
