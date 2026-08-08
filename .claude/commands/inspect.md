---
name: inspect
description: Audit inspector artifacts and read explicitly activated session metadata
---

Use the installed `pulp` command. It delegates to the installed `pulp-cpp`
sibling; do not rely on a source-tree build path.

Run the read-only evidence loop:

```bash
pulp inspect audit PATH --json
pulp inspect doctor --json
pulp inspect profiles --json
pulp inspect list --json
pulp inspect capabilities --json \
  --session SESSION_ID --instance INSTANCE_ID --publication PUBLICATION_ID
```

Keep all three identities from one `list` record together. A changed or missing
publication requires rediscovery. Report the selected profile and effective
capabilities without inferring authority from the profile name.

The shipped command deliberately exposes no generic method, mutation,
screenshot, host, or port route. Use the canonical broker/control platform for
authorized live operations; do not recreate a raw inspector fallback.
