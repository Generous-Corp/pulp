---
name: inspect
description: Use the experimental client with an explicitly activated Development Inspector
---

`pulp inspect` is the low-level authenticated client for an explicitly
activated Development Inspector. Start a GPU-enabled desktop standalone with
`pulp run --inspect` (or another profile); normal `pulp run`, GPU-off/mobile
builds, and plugin-format launches do not start an endpoint.

```bash
./build/pulp inspect
./build/pulp inspect --port 49152
./build/pulp inspect --command DOM.getDocument
./build/pulp inspect --command State.getParameters
```

`Capture.screenshot` captures the selected standalone window when its host
provides compositor capture. `Capture.screenshotNode` remains unavailable.
`Runtime.evaluate` is unavailable in normal launches. An explicitly wired custom fixture
can enable it; treat that opt-in as remote code execution.

The inspector exposes:
- View hierarchy with bounds, flex properties, and styles
- Widget state (values, labels, visibility)
- Theme tokens and computed colors
- Layout debug information

Auto-discovery reads owner-private ephemeral records and credentials, selects
an exact session/instance/publication generation, and authenticates both peers
with role-separated per-connection nonce/HMAC proofs bound to that generation.
Mutations additionally require the session's controller lease.
