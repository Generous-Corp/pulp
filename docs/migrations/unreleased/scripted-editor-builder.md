---
breaking = true
summary = "A scripted editor declared only through PluginRegistration::ui_script needs install_scripted_editor_builder()"
---

## What changed

The code that builds a scripted editor from declared editor assets now links only
into binaries that ask for it. A target that declares `UI_SCRIPT` (or gets a
script from `pulp_use_kit_ui()`) asks automatically through its generated
editor-assets source, so nothing changes for it. Plug-ins whose editor is
AutoUi no longer link the script runtime, which takes about 5 MB out of each
stripped module.

A multi-plug-in bundle whose scripts come only from hand-written
`PluginRegistration::ui_script` entries, with no `UI_SCRIPT` on any of its
targets, does not ask. Its editors now open AutoUi and log:

```
Scripted editor UI '<path>' not built: this binary declares a scripted editor but does not link the script runtime; ...
```

`ViewBridge::last_error()` carries the same reason.

## Migration steps

1. Call the installer once in that bundle, for example next to the
   `register_plugin()` calls:

   ```cpp
   pulp::format::install_scripted_editor_builder();
   ```

   Or declare `UI_SCRIPT` on one of the bundle's format targets.
2. Open an editor and confirm the log line above is gone.
