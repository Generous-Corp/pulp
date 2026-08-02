---
name: inspect
description: Use the experimental inspector client with an explicitly hosted fixture
---

`pulp inspect` is currently a low-level client, not a normal `pulp run` or
plugin-format workflow. Normal Pulp launches do not start an inspector
endpoint. Use these commands only with a custom host/test fixture that
explicitly constructs `InspectorServer`.

```bash
./build/pulp inspect
./build/pulp inspect --port 49152
./build/pulp inspect --command DOM.getDocument
./build/pulp inspect --command State.getParameters
```

`Capture.screenshot` and `Capture.screenshotNode` currently return explicit
unavailable errors until host-capture wiring lands. `Runtime.evaluate` is
unavailable in normal launches, but an explicitly wired custom fixture can
enable it; treat that opt-in as remote code execution.

The inspector exposes:
- View hierarchy with bounds, flex properties, and styles
- Widget state (values, labels, visibility)
- Theme tokens and computed colors
- Layout debug information

Auto-discovery reads owner-private ephemeral records and credentials, selects
an exact session/instance/publication generation, and authenticates both peers
with role-separated per-connection nonce/HMAC proofs bound to that generation.
Mutations additionally require the session's controller lease.
