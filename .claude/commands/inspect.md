---
name: inspect
description: Read static inspector profiles and audit inspector artifacts offline
---

Use the installed `pulp` command. It delegates to the installed `pulp-cpp`
sibling; do not rely on a source-tree build path.

The Phase 3 surface is intentionally small:

```bash
pulp control profiles --json
pulp inspect audit PATH --json
```

`pulp inspect profiles` remains a deprecated alias through Pulp 0.800.0 on
2026-10-01. `profiles` reads the compiled capability registry. `audit` reads an artifact and
its canonical manifest without loading, executing, connecting to, or activating
the artifact.

The shipped command exposes no discovery, live capability query, generic
method, mutation, screenshot, host, port, or publication-selector route. This
temporary capability reduction is intentional while Phases 4–7 replace the
deleted legacy authority with the canonical broker, trusted launcher/host
adapters, execution routing, and shipping evidence. Do not recreate a raw
Inspector fallback.
