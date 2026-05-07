# What version is Pulp?

Short answer:

> **Pulp's version = `project(Pulp VERSION …)` in `CMakeLists.txt`.** Today that is the number released on the GitHub Releases page. Everything else is a derivative surface.

## The three surfaces

Pulp ships three things that move independently. Each has its own version, and `tools/scripts/versioning.json` is the single authority for which file holds which number.

| Surface             | Source of truth                     | Bumped by                                    |
|---------------------|-------------------------------------|----------------------------------------------|
| **Pulp SDK / CLI**  | `CMakeLists.txt` `project(Pulp VERSION X.Y.Z)` | `pulp pr` via `tools/scripts/version_bump_check.py --mode=apply` |
| **Claude Code plugin** | `.claude-plugin/plugin.json` + `.claude-plugin/marketplace.json` | same (plugin-scoped) |
| **Shipyard** (operational dep) | `tools/shipyard.toml` → `version = "…"` | `tools/install-shipyard.sh`, manual bump |

When someone asks "what version of Pulp?" they almost always mean the **SDK / CLI** number. The CLI binary embeds it (`pulp --version`) and every Pulp project inherits it through the CMake find-package exports.

## Why the plugin has its own number

The Claude Code plugin is a separate distribution surface: it can ship a new UX iteration (new slash-commands, refined agents) without the SDK moving, and vice-versa. The versioning split is intentional — conflating them would force every C++ API bump to also cut a Claude plugin release. The plugin's number is a plugin-API contract with Claude Code, not a Pulp-API contract with plug-in developers.

## Why shipyard has its own pin

Shipyard is Pulp's CI controller and ships on its own cadence. Pulp pins a specific release in `tools/shipyard.toml` so every worktree exercises the same validation behaviour; bumps are a dependency update, not a Pulp feature release.
It is not part of the public Pulp CLI installer and is not treated as a
runtime/build dependency for Pulp users; it is a source-checkout contributor
tool installed explicitly with `./tools/install-shipyard.sh`.

## How to surface the current SDK version

### From a shell
```bash
pulp version       # prints the SDK / CLI version
pulp --version     # same, POSIX-shortcut form
```

### From CMake in a downstream project
```cmake
find_package(Pulp CONFIG REQUIRED)
message(STATUS "Building against Pulp ${Pulp_VERSION}")
```

### From the Claude Code plugin
The `/status` and `/doctor` slash-commands both print the SDK version in their output, alongside the Claude plugin version. A dedicated `/version` command prints both on a single line:

```
Claude plugin 0.5.0 · Pulp SDK/CLI 0.13.0
```

This double-line answer is the canonical reply to "what version is my project using?"

## When does a new GitHub Release appear?

Every merge to `main` that moves `project(Pulp VERSION …)` triggers `.github/workflows/auto-release.yml`, which tags the new version. The tag push fires `release-cli.yml` which builds the CLI binaries and publishes a release at `https://github.com/danielraffel/pulp/releases/tag/vX.Y.Z`.

If `auto-release.yml` sees `SDK_BEFORE == SDK_AFTER` (version didn't move on that merge), no tag is created — that is by design so doc-only PRs don't cut releases. The Releases page lags the CMakeLists version briefly whenever a batch of non-version-bumping PRs lands in a row.

## Troubleshooting "the Releases page looks stale"

- Check `tools/shipyard.toml` is pinned to a current shipyard version (`./tools/install-shipyard.sh --status`).
- Check that `auto-release.yml` has run for the most recent version-bumping merge (`gh run list --workflow=auto-release.yml`).
- Confirm the tip commit doesn't carry a `Release: skip …` trailer — that intentionally suppresses tagging.
- Release workflows run on tag push, so a failed `release-cli.yml` run leaves the tag in place but no Release published; in that case re-running the workflow from the Actions UI is the fix.

### The two `fatal: could not read Username` root causes

If `auto-release.yml` fails at the `actions/checkout` step with `fatal: could not read Username for 'https://github.com': No such device or address`, there are **two independent causes that often compound**:

1. **PAT scope** — the fine-grained `RELEASE_BOT_TOKEN` PAT is missing this repo in its *Selected repositories* list. Fine-grained PATs are strictly scoped; reusing the same token across multiple repos requires listing every consumer up front (or cutting a separate PAT per repo). Fix: edit the token at <https://github.com/settings/personal-access-tokens>, add the repo, save — no secret rotation needed.
2. **Secret value drift** — the `RELEASE_BOT_TOKEN` secret on the repo holds a different token value than the one you edited. This happens when the secret was seeded from an earlier PAT that was later revoked or replaced. Diagnose by comparing `gh secret list` timestamp against the PAT's regeneration time; resolve by running `gh secret set RELEASE_BOT_TOKEN` with the current token.

Both scenarios surface the same error message, so rule them out in order. See shipyard's [`RELEASING.md`](https://github.com/danielraffel/Shipyard/blob/main/RELEASING.md) (clarified in shipyard [#45](https://github.com/danielraffel/Shipyard/pull/45) + [#46](https://github.com/danielraffel/Shipyard/pull/46)) for the provisioning walk-through; guided PAT provisioning to prevent the mis-setup is tracked in shipyard [#48](https://github.com/danielraffel/Shipyard/issues/48).

## See also
- `tools/scripts/versioning.json` — machine-readable config consumed by `version_bump_check.py` and `skill_sync_check.py`.
- `docs/guides/versioning.md` — author-facing guide on how `pulp pr` drives the whole pipeline, including bypass trailers.
- `docs/guides/shipyard.md` — CI controller deep-dive.
