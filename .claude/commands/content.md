# /content

Validate, preview, install, update, list, rescan, remove, or reveal data-only Pulp content packs.

Use this for `.pulpcontent` archives or `content-pack` directories that contain presets, themes, samples, wavetables, or other user-facing data for an installed plugin.

Why it matters:

- plugin authors ship expansion packs without custom installers;
- users can validate, see reload/restart expectations, and remove packs without losing their own presets;
- plugins discover supported content through declared runtime capabilities.

Content packs are not dependency packages and are not project-transforming kits.

Common commands:

```bash
pulp content validate <path> --json
pulp content preview <path> --plugin-runtime <manifest> --json
pulp content install <path> --plugin <plugin-id> --yes
pulp content update <path> --plugin <plugin-id> --yes
pulp content list --plugin <plugin-id> --json
pulp content rescan --json
pulp content reveal <package-id> --plugin <plugin-id> --version <version>
pulp content remove <package-id> --plugin <plugin-id> --version <version> --yes
```

Workflow:

1. Validate the pack.
2. Preview against the trusted plugin runtime manifest when available.
3. Explain target plugin, supported kinds, install path, and hot-reload/rescan/restart policy.
4. Install or update only after explicit approval.
5. Use `rescan` to rebuild the local content index after manual changes or repair; index entries include the target `plugin_id` and installed manifest digest for audit/drift checks.
6. Remove only the installed content-pack root.

Trust rules:

- content commands copy data only; they never run package CMake, JavaScript, scripts, dynamic libraries, or remote fetches;
- `.pulpcontent` archives must include `files.sha256.json`, and every payload file must be listed and hash-matched before preview/install/update;
- `preview` reads a trusted plugin runtime manifest and reports compatibility plus hot-reload/rescan/restart policy;
- install/update/remove require explicit approval and safe single-component plugin/package/version ids;
- removal deletes only the installed content-pack root; user-created presets must survive.

Runtime plugins opt in through `ContentRegistry` or `PresetManager`, preferably from an embedded `pulp.plugin-runtime.json`. In-app installers and drop handlers should call `preview_content_pack_install(...)`, show the target path and reload policy, then call `install_content_pack(..., approved=true)` only after approval.

Plugin authors should declare content support in CMake:

```cmake
pulp_add_plugin(MySynth
    ...
    CONTENT_CAPABILITIES content.presets.v1
    CONTENT_KINDS presets
    CONTENT_HOT_RELOAD_KINDS presets)
```

That generates and bundles `pulp.plugin-runtime.json`, so agents and validators can tell which packs match the plugin and whether users need hot reload, rescan, or restart before install.
