---
name: inspect
description: Connect to a running Pulp inspector server
---

Connect to a running plugin's inspector server to query the view tree,
widget state, layout, and runtime diagnostics.

```bash
./build/tools/cli/pulp inspect
./build/tools/cli/pulp inspect --port 49152
./build/tools/cli/pulp inspect --command DOM.getDocument
./build/tools/cli/pulp inspect --command Capture.screenshot --output shot.json
```

The inspector exposes:
- View hierarchy with bounds, flex properties, and styles
- Widget state (values, labels, visibility)
- Theme tokens and computed colors
- Layout debug information

Use `--port` when auto-discovery cannot find a running inspector.
