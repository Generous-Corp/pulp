# AGENTS.md

Instructions for AI agents (Codex, Claude Code, etc.) working in this repo.

## Docs Maintenance Rule

When you modify files in `core/`, `examples/`, or `tools/cli/`:

1. Check if the change affects public behavior, supported formats, module dependencies, or CLI commands
2. If yes, update the relevant YAML manifests in `docs/status/`:
   - `support-matrix.yaml` — format/platform support levels
   - `modules.yaml` — module status, dependencies
   - `cli-commands.yaml` — CLI command descriptions
   - `cmake-functions.yaml` — CMake function signatures
3. If yes, update the relevant Markdown docs in `docs/`:
   - `reference/modules.md` — module descriptions
   - `reference/cli.md` — CLI reference
   - `reference/capabilities.md` — capability listings
   - Example pages in `docs/examples/` if examples changed
4. Run `tools/check-docs.sh` (or `pulp docs check`) to validate consistency

## Status Vocabulary

Use only these values for `status:` fields in manifests:
`stable`, `usable`, `experimental`, `partial`, `planned`, `unsupported`

## Clean-Room Rule

Never reference JUCE source code. See CLAUDE.md for details.

## Testing

Run `ctest --test-dir build --output-on-failure` after code changes.
Run `tools/check-docs.sh` after docs/manifest changes.

## Commit Standards

- Imperative mood, explain why not just what
- No WIP/fix/misc commits on main
- Every commit should build and pass tests
