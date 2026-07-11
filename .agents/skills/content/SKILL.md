---
name: content
description: Validate, install, update, list, rescan, remove, and reveal data-only Pulp content packs for installed plugins. Use for presets, themes, samples, wavetables, and other end-user data that must be reviewed before install or update.
requires:
  scripts:
    - tools/kits/pulp-package.schema.json
    - tools/kits/pulp-plugin-runtime.schema.json
---

# Pulp Content

Use this skill when a user has a `.pulpcontent` archive or local `content-pack` directory for an installed plugin.

Content packs are valuable because:

- plugin authors ship presets, themes, samples, and wavetables without custom installers;
- users get validation, a visible target plugin/content path, and reversible removal;
- runtime plugins consume installed data through `ContentRegistry` or `PresetManager` content capabilities.

## Trust Boundary

`pulp add <name>` is for curated developer dependency packages.

`pulp kit ...` is for developer artifacts that may transform a project.

`pulp content ...` is for end-user data installed into a plugin-specific content directory.

Trust rules:

- content commands copy data only; they never run package CMake, JavaScript, scripts, dynamic libraries, or network fetches;
- `.pulpcontent` archives must include `files.sha256.json`, and every payload file must be listed and hash-matched before preview/install/update;
- update uses an explicit local path, not registry resolution, and rolls back a replaced version on failure;
- install/update/remove/reveal require safe single-component plugin/package/version ids;
- rescan is metadata-only and records `plugin_id` plus `manifest_sha256`;
- removal deletes only the installed pack root; user edits stay in the plugin's normal user preset path.

Built plugins should embed `pulp.plugin-runtime.json` via `pulp_add_plugin(... CONTENT_CAPABILITIES ... CONTENT_KINDS ...)`. Agents and in-app installers should preview with that manifest before approval, then install with `approved=true`. Validate built bundles with `ValidationHarness::validate_plugin_runtime_manifest(...)`.

## Commands

```bash
./build/pulp content validate <path> --json
./build/pulp content preview <path> --plugin-runtime <manifest> --json
./build/pulp content install <path> --plugin <plugin-id> --yes
./build/pulp content update <path> --plugin <plugin-id> --yes
./build/pulp content list --plugin <plugin-id> --json
./build/pulp content rescan --json
./build/pulp content reveal <package-id> --plugin <plugin-id> --version <version>
./build/pulp content remove <package-id> --plugin <plugin-id> --version <version> --yes
```

Use `--root <dir>` only for tests, CI, or explicit sandboxing. Normal installs use the platform user-data root.

## Agent Workflow

1. Validate the pack.
2. Preview against the trusted plugin runtime manifest when available, and explain content id, version, capabilities, target plugin, supported kinds, and reload/restart policy.
3. Preview the install root using `reveal` or the install plan text.
4. Install or update only after explicit approval with `--yes`.
5. Use `rescan` when the index is missing/stale or after manual repair.
6. Remove only after explicit approval with `--yes`.
7. Never delete user-created presets or files outside the installed pack root.

Use MCP tools when available:

- `pulp_content_validate`
- `pulp_content_preview`
- `pulp_content_install`
- `pulp_content_update`
- `pulp_content_list`
- `pulp_content_rescan`
- `pulp_content_remove`
- `pulp_content_reveal`

## Related extend surfaces

`packages`, `kits`, `content`, and `installable-tools` are Pulp's four ways to
extend a project or machine, and they share one lifecycle contract: **add is
validated, remove is confirmed + confined to the surface's own area + names what
it deleted, and both add and remove ship tests.** Pick the right surface and read
the shared contract in
[extending-pulp.md](../../../docs/reference/extending-pulp.md).

- [`packages`](../packages/SKILL.md) — third-party audio DSP libraries → a project
- [`kits`](../kits/SKILL.md) — reusable Pulp code/UI/templates → a project
- [`content`](../content/SKILL.md) — data-only packs (presets/samples) → an installed plugin
- [`installable-tools`](../installable-tools/SKILL.md) — machine-level dev/agent tooling under `~/.pulp/tools/`, plus the shared validate-and-uninstall-from-outside-a-checkout bar
