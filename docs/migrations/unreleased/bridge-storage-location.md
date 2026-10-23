---
breaking = true
summary = "Scripted-UI localStorage and style presets move from the temp directory to a per-user, per-bridge store"
---

## Breaking changes
- `storageGetItem` / `storageSetItem` / `storageRemoveItem` (and the `localStorage` shim) store entries under the per-user settings root, namespaced by the bridge's script base directory, with the key hashed. Entries previously written to `$TMPDIR/pulp-storage` are not migrated, and two plugins no longer share one store.
- `saveStylePreset` / `loadStylePreset` move from `$TMPDIR/pulp-style-presets` to the same per-user store.
- Preset names and folders passed to `PresetManager::save` / `rename` may no longer contain `..`, an empty component, or a Windows separator; nested folders such as `Keys/Analog` still work.

## Migration steps
- Treat values that lived in the temp directory as ephemeral (the OS already purged them on reboot); nothing to migrate.
- A script that shared a store across plugins should persist shared state through the plugin's own state instead.
